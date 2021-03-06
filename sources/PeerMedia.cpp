/*
Copyright 2016 Thomas Jammet
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This file is part of Librtmfp.

Librtmfp is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Librtmfp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Librtmfp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "PeerMedia.h"
#include "RTMFPWriter.h"
#include "P2PSession.h"

using namespace Mona;
using namespace std;

PeerMedia::PeerMedia(P2PSession* pSession, shared_ptr<RTMFPWriter>& pMediaReportWriter) : _pMediaReportWriter(pMediaReportWriter), _pParent(pSession), _idFragmentsMapIn(0), _idFragmentsMapOut(0), 
	idFlow(0), idFlowMedia(0), pStreamKey(NULL), _pushOutMode(0), pushInMode(0), groupMediaSent(false), _fragmentsMap(MAX_FRAGMENT_MAP_SIZE) {

}

PeerMedia::~PeerMedia() {

	close(true);
	_pParent = NULL;
}

void PeerMedia::close(bool abrupt) {
	closeMediaWriter(abrupt);
	if (_pMediaReportWriter) {
		if (!abrupt) {
			_pParent->closeFlow(idFlow);
			_pMediaReportWriter->close();
		}
		idFlow = 0;
		_pMediaReportWriter.reset();
	}

	OnPeerClose::raise(_pParent->peerId, pushInMode); // notify GroupMedia to reset push masks and remove pointer
}

void PeerMedia::closeMediaWriter(bool abrupt) {
	if (_pMediaWriter) {
		if (!abrupt) {
			_pParent->closeFlow(idFlowMedia);
			_pMediaWriter->close();
		}
		idFlowMedia = 0;
		_pMediaWriter.reset();
	}
}

void PeerMedia::setMediaWriter(shared_ptr<RTMFPWriter>& pWriter) {
	_pMediaWriter = pWriter;
}

void PeerMedia::flushReportWriter() {
	if (_pMediaReportWriter)
		_pMediaReportWriter->flush();
}

void PeerMedia::sendGroupMedia(const string& stream, const std::string& streamKey, RTMFPGroupConfig* groupConfig) {
	TRACE("Sending the Media Subscription for stream '", stream, "' to peer ", _pParent->peerId)

	_pMediaReportWriter->writeGroupMedia(stream, BIN streamKey.data(), streamKey.size(), groupConfig);
	groupMediaSent = true;
}

bool PeerMedia::sendMedia(const UInt8* data, UInt32 size, UInt64 fragment, bool pull) {
	if ((!pull && !isPushable((UInt8)fragment%8)))
		return false;

	if (!_pMediaWriter && !_pParent->createMediaWriter(_pMediaWriter, idFlow)) {
		ERROR("Unable to create media writer for peer ", _pParent->peerId)
		return false;
	}

	_pMediaWriter->writeRaw(data, size);
	_pMediaWriter->flush();
	return true;
}

bool PeerMedia::sendFragmentsMap(UInt64 lastFragment, const UInt8* data, UInt32 size) {
	if (_pMediaReportWriter && lastFragment != _idFragmentsMapOut) {
		DEBUG("Sending Fragments Map message (type 22) to peer ", _pParent->peerId, " (", lastFragment,")")
		_pMediaReportWriter->writeRaw(data, size);
		_pMediaReportWriter->flush();
		_idFragmentsMapOut = lastFragment;
		return true;
	}
	return false;
}

void PeerMedia::setPushMode(UInt8 mode) {
	_pushOutMode = mode;
}

bool PeerMedia::isPushable(UInt8 rest) {
	return (_pushOutMode & (1 << rest)) > 0;
}

void PeerMedia::sendPushMode(UInt8 mode) {
	if (_pMediaReportWriter && pushInMode != mode) {
		string masks;
		if (mode > 0) {
			for (int i = 0; i < 8; i++) {
				if ((mode & (1 << i)) > 0)
					String::Append(masks, (masks.empty() ? "" : ", "), i, ", ", Format<UInt8>("%.1X", i + 8));
			}
		}

		DEBUG("Setting Group Push In mode to ", Format<UInt8>("%.2x", mode), " (", masks,") for peer ", _pParent->peerId, " - last fragment : ", _idFragmentsMapIn)
		_pMediaReportWriter->writeGroupPlay(mode);
		_pMediaReportWriter->flush();
		pushInMode = mode;
	}
}

void PeerMedia::onFragmentsMap(UInt64 id, const UInt8* data, UInt32 size) {
	// If the group is publisher for this media we ignore the request
	if (!OnFragmentsMap::raise<true>(id))
		return;

	if (id <= _idFragmentsMapIn) {
		DEBUG("Wrong Group Fragments map received from peer ", _pParent->peerId, " : ", id, " <= ", _idFragmentsMapIn)
		return;
	}

	_idFragmentsMapIn = id;
	if (!size)
		return; // 0 size protection

	if (size > MAX_FRAGMENT_MAP_SIZE)
		WARN("Size of fragment map > max size : ", size)
	_fragmentsMap.resize(size);
	BinaryWriter writer(_fragmentsMap.data(), size);
	writer.write(data, size);
}

void PeerMedia::onFragment(UInt8 marker, UInt64 id, UInt8 splitedNumber, UInt8 mediaType, UInt32 time, PacketReader& packet, double lostRate) {
	OnFragment::raise(this, _pParent->peerId, marker, id, splitedNumber, mediaType, time, packet, lostRate);
}

bool PeerMedia::checkMask(UInt8 bitNumber) {
	if (!_idFragmentsMapIn)
		return false;

	if (_idFragmentsMapIn % 8 == bitNumber)
		return true;

	// Determine the last fragment with bit mask
	UInt64 lastFragment = _idFragmentsMapIn - (_idFragmentsMapIn % 8);
	lastFragment += ((_idFragmentsMapIn % 8) > bitNumber) ? bitNumber : bitNumber - 8;

	DEBUG("Searching ", lastFragment, " into ", Format<UInt8>("%.2x", *_fragmentsMap.data()), " ; (current id : ", _idFragmentsMapIn, ") ; result = ",
		((*_fragmentsMap.data()) & (1 << (8 - _idFragmentsMapIn + lastFragment))) > 0, " ; bit : ", bitNumber, " ; address : ", _pParent->peerId, " ; latency : ", _pParent->latency())

	return ((*_fragmentsMap.data()) & (1 << (8 - _idFragmentsMapIn + lastFragment))) > 0;
}

bool PeerMedia::hasFragment(UInt64 index) {
	if (!_idFragmentsMapIn || (_idFragmentsMapIn < index)) {
		TRACE("Searching ", index, " impossible into ", _pParent->peerId, ", current id : ", _idFragmentsMapIn)
		return false; // No Fragment or index too recent
	}
	else if (_idFragmentsMapIn == index) {
		TRACE("Searching ", index, " OK into ", _pParent->peerId, ", current id : ", _idFragmentsMapIn)
		return true; // Fragment is the last one or peer has all fragments
	}
	else if (_blacklistPull.find(index) != _blacklistPull.end()) {
		TRACE("Searching ", index, " impossible into ", _pParent->peerId, " a request has already failed")
		return false;
	}

	UInt32 offset = (UInt32)((_idFragmentsMapIn - index - 1) / 8);
	UInt32 rest = ((_idFragmentsMapIn - index - 1) % 8);
	if (offset > _fragmentsMap.size()) {
		TRACE("Searching ", index, " impossible into ", _pParent->peerId, ", out of buffer (", offset, "/", _fragmentsMap.size(), ")")
		return false; // Fragment deleted from buffer
	}

	TRACE("Searching ", index, " into ", Format<UInt8>("%.2x", *(_fragmentsMap.data() + offset)), " ; (current id : ", _idFragmentsMapIn, ", offset : ", offset, ") ; result = ",
		(*(_fragmentsMap.data() + offset) & (1 << rest)) > 0)

	return (*(_fragmentsMap.data() + offset) & (1 << rest)) > 0;
}

void PeerMedia::onPlayPull(UInt64 index) {

	OnPlayPull::raise(this, index);
}

void PeerMedia::sendPull(UInt64 index) {
	if (!_pMediaReportWriter)
		return;

	TRACE("Sending pull request for fragment ", index, " to peer ", _pParent->peerId);
	_pMediaReportWriter->writeGroupPull(index);
}

void PeerMedia::addPullBlacklist(UInt64 idFragment) {
	// TODO: delete old blacklisted fragments
	_blacklistPull.emplace(idFragment);
}
