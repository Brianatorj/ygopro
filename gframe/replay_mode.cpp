#include "replay_mode.h"
#include "duelclient.h"
#include "game.h"
#include "single_mode.h"
#include "../ocgcore/duel.h"
#include "../ocgcore/field.h"
#include "../ocgcore/mtrandom.h"

namespace ygo {

long ReplayMode::pduel = 0;
Replay ReplayMode::cur_replay;
std::vector<BufferIO::ReplayPacket> ReplayMode::current_stream;
bool ReplayMode::is_continuing = true;
bool ReplayMode::is_closing = false;
bool ReplayMode::is_pausing = false;
bool ReplayMode::is_paused = false;
bool ReplayMode::is_swapping = false;
bool ReplayMode::is_restarting = false;
bool ReplayMode::exit_pending = false;
int ReplayMode::skip_turn = 0;
int ReplayMode::current_step = 0;
int ReplayMode::skip_step = 0;

bool ReplayMode::StartReplay(int skipturn) {
	skip_turn = skipturn;
	if(skip_turn < 0)
		skip_turn = 0;
	Thread::NewThread(ReplayThread, 0);
	return true;
}
void ReplayMode::StopReplay(bool is_exiting) {
	is_pausing = false;
	is_continuing = false;
	is_closing = is_exiting;
	exit_pending = true;
	mainGame->actionSignal.Set();
}
void ReplayMode::SwapField() {
	if(is_paused)
		mainGame->dField.ReplaySwap();
	else
		is_swapping = true;
}
void ReplayMode::Pause(bool is_pause, bool is_step) {
	if(is_pause)
		is_pausing = true;
	else {
		if(!is_step)
			is_pausing = false;
		mainGame->actionSignal.Set();
	}
}
bool ReplayMode::ReadReplayResponse() {
	unsigned char resp[64];
	bool result = cur_replay.ReadNextResponse(resp);
	if(result)
		set_responseb(pduel, resp);
	return result;
}
int ReplayMode::ReplayThread(void* param) {
	const ReplayHeader& rh = cur_replay.pheader;
	mainGame->dInfo.isFirst = true;
	mainGame->dInfo.isTag = !!(rh.flag & REPLAY_TAG);
	mainGame->dInfo.isSingleMode = !!(rh.flag & REPLAY_SINGLE_MODE);
	mainGame->dInfo.lua64 = true;
	mainGame->dInfo.tag_player[0] = false;
	mainGame->dInfo.tag_player[1] = false;
	if (mainGame->dInfo.isTag) {
		cur_replay.ReadName(mainGame->dInfo.hostname);
		cur_replay.ReadName(mainGame->dInfo.hostname_tag);
		cur_replay.ReadName(mainGame->dInfo.clientname_tag);
		cur_replay.ReadName(mainGame->dInfo.clientname);
	}
	else {
		cur_replay.ReadName(mainGame->dInfo.hostname);
		cur_replay.ReadName(mainGame->dInfo.clientname);
	}
	int opt = cur_replay.ReadInt32();
	mainGame->dInfo.duel_field = opt & 0xff;
	mainGame->dInfo.extraval = (opt >> 8);
	mainGame->SetPhaseButtons();
	BufferIO::ReplayPacket p;
	current_stream.clear();
	while (cur_replay.ReadNextPacket(&p)) {
		current_stream.push_back(p);
	}
	mainGame->dInfo.isStarted = true;
	mainGame->dInfo.isReplay = true;
	mainGame->dInfo.isReplaySkiping = (skip_turn > 0);
	is_continuing = true;
	skip_step = 0;
	exit_pending = false;
	current_step = 0;
	if(mainGame->dInfo.isReplaySkiping)
		mainGame->gMutex.Lock();
	for(auto it = current_stream.begin(); is_continuing && !exit_pending && it != current_stream.end();) {
		is_continuing = ReplayAnalyze((*it));
		if(is_restarting) {
			it = current_stream.begin();
			is_restarting = false;
			int step = current_step - 1;
			if (step < 0)
				step = 0;
			if (step == 0) {
				Pause(true, false);
				mainGame->dInfo.isStarted = true;
				mainGame->dInfo.isReplaySkiping = false;
				mainGame->dField.RefreshAllCards();
				mainGame->gMutex.Unlock();
			}
			skip_step = step;
			current_step = 0;
		} else
			it++;
	}
	if(mainGame->dInfo.isReplaySkiping) {
		mainGame->dInfo.isReplaySkiping = false;
		mainGame->dField.RefreshAllCards();
		mainGame->gMutex.Unlock();
	}
	EndDuel();
	return 0;
}
void ReplayMode::EndDuel() {
	if(!is_closing) {
		mainGame->actionSignal.Reset();
		mainGame->gMutex.Lock();
		mainGame->stMessage->setText(dataManager.GetSysString(1501));
		if(mainGame->wCardSelect->isVisible())
			mainGame->HideElement(mainGame->wCardSelect);
		mainGame->PopupElement(mainGame->wMessage);
		mainGame->gMutex.Unlock();
		mainGame->actionSignal.Wait();
		mainGame->gMutex.Lock();
		mainGame->dInfo.isStarted = false;
		mainGame->dInfo.isReplay = false;
		mainGame->gMutex.Unlock();
		mainGame->closeDoneSignal.Reset();
		mainGame->closeSignal.Set();
		mainGame->closeDoneSignal.Wait();
		mainGame->gMutex.Lock();
		mainGame->ShowElement(mainGame->wReplay);
		mainGame->stTip->setVisible(false);
		mainGame->device->setEventReceiver(&mainGame->menuHandler);
		mainGame->gMutex.Unlock();
		if(exit_on_return)
			mainGame->device->closeDevice();
	}
}
void ReplayMode::Restart(bool refresh) {
	mainGame->dInfo.isStarted = false;
	mainGame->dField.Clear();
	mainGame->dInfo.tag_player[0] = false;
	mainGame->dInfo.tag_player[1] = false;
	if(refresh) {
		mainGame->dField.RefreshAllCards();
		mainGame->dInfo.isStarted = true;
	}
	skip_turn = 0;
	is_restarting = true;
}
void ReplayMode::Undo() {
	if(skip_step > 0 || current_step == 0)
		return;
	mainGame->dInfo.isReplaySkiping = true;
	Restart(false);
	mainGame->gMutex.Lock();
	Pause(false, false);
}
bool ReplayMode::ReplayAnalyze(BufferIO::ReplayPacket p) {
	is_restarting = false;
	while(true) {
		if(is_closing)
			return false;
		if(is_restarting)
			break;
		if(is_swapping) {
			mainGame->gMutex.Lock();
			mainGame->dField.ReplaySwap();
			mainGame->gMutex.Unlock();
			is_swapping = false;
		}
		bool pauseable = true;
		mainGame->dInfo.curMsg = p.message;
		switch (mainGame->dInfo.curMsg) {
		case MSG_RETRY: {
			if(mainGame->dInfo.isReplaySkiping) {
				mainGame->dInfo.isReplaySkiping = false;
				mainGame->dField.RefreshAllCards();
				mainGame->gMutex.Unlock();
			}
			mainGame->gMutex.Lock();
			mainGame->stMessage->setText(L"Error occurs.");
			mainGame->PopupElement(mainGame->wMessage);
			mainGame->gMutex.Unlock();
			mainGame->actionSignal.Reset();
			mainGame->actionSignal.Wait();
			return false;
		}
		case MSG_WIN: {
			if (mainGame->dInfo.isReplaySkiping) {
				mainGame->dInfo.isReplaySkiping = false;
				mainGame->dField.RefreshAllCards();
				mainGame->gMutex.Unlock();
			}
			DuelClient::ClientAnalyze((char*)p.data, p.length);
			return false;
		}
		case MSG_START:
		case MSG_UPDATE_DATA:
		case MSG_UPDATE_CARD:
		case MSG_SET:
		case MSG_SWAP:
		case MSG_FIELD_DISABLED:
		case MSG_SUMMONING:
		case MSG_SPSUMMONING:
		case MSG_FLIPSUMMONING:
		case MSG_CHAIN_SOLVING:
		case MSG_CHAIN_SOLVED:
		case MSG_CHAIN_END:
		case MSG_CARD_SELECTED:
		case MSG_RANDOM_SELECTED:
		case MSG_EQUIP:
		case MSG_UNEQUIP:
		case MSG_CARD_TARGET:
		case MSG_CANCEL_TARGET:
		case MSG_BATTLE:
		case MSG_ATTACK_DISABLED:
		case MSG_DAMAGE_STEP_START:
		case MSG_DAMAGE_STEP_END:
		case MSG_TAG_SWAP:
		case MSG_RELOAD_FIELD: {
			pauseable = false;
			break;
		}
		case MSG_NEW_TURN: {
			if(skip_turn) {
				skip_turn--;
				if(skip_turn == 0) {
					mainGame->dInfo.isReplaySkiping = false;
					mainGame->dField.RefreshAllCards();
					mainGame->gMutex.Unlock();
				}
			}
			break;
		}
		case OLD_REPLAY_MODE:
			break;
		}
		DuelClient::ClientAnalyze((char*)p.data, p.length);
		if(pauseable) {
			current_step++;
			if(skip_step) {
				skip_step--;
				if(skip_step == 0) {
					Pause(true, false);
					mainGame->dInfo.isStarted = true;
					mainGame->dInfo.isReplaySkiping = false;
					mainGame->dField.RefreshAllCards();
					mainGame->gMutex.Unlock();
				}
			}
			if(is_pausing) {
				is_paused = true;
				mainGame->actionSignal.Reset();
				mainGame->actionSignal.Wait();
				is_paused = false;
			}
		}
		break;
	}
	return true;
}

}
