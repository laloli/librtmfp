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

#include "GroupMedia.h"
#include "NetGroup.h"
#include "GroupStream.h"
#include "librtmfp.h"

using namespace Mona;
using namespace std;

// Fragment instance
class MediaPacket : public virtual Object {
public:
	MediaPacket(const PoolBuffers& poolBuffers, const UInt8* data, UInt32 size, UInt32 totalSize, UInt32 time, AMF::ContentType mediaType,
		UInt64 fragmentId, UInt8 groupMarker, UInt8 splitId) : splittedId(splitId), type(mediaType), marker(groupMarker), time(time), pBuffer(poolBuffers, totalSize) {
		BinaryWriter writer(pBuffer->data(), totalSize);

		// AMF Group marker
		writer.write8(marker);
		// Fragment Id
		writer.write7BitLongValue(fragmentId);
		// Splitted sequence number
		if (splitId > 0)
			writer.write8(splitId);

		// Type and time, only for the first fragment
		if (marker != GroupStream::GROUP_MEDIA_NEXT && marker != GroupStream::GROUP_MEDIA_END) {
			// Media type
			writer.write8(type);
			// Time on 4 bytes
			writer.write32(time);
		}
		// Payload
		payload = writer.data() + writer.size(); // TODO: check if it is the correct pos
		writer.write(data, size);
	}

	UInt32 payloadSize() { return pBuffer.size() - (payload - pBuffer.data()); }

	PoolBuffer			pBuffer;
	UInt32				time;
	AMF::ContentType	type;
	const UInt8*		payload; // Payload position
	UInt8				marker;
	UInt8				splittedId;
};

Buffer	GroupMedia::_fragmentsMapBuffer;
UInt32	GroupMedia::GroupMediaCounter = 0;

GroupMedia::GroupMedia(const PoolBuffers& poolBuffers, const string& name, const string& key, std::shared_ptr<RTMFPGroupConfig> parameters) : _fragmentCounter(0), _firstPushMode(true), _currentPushMask(0), 
	_currentPullFragment(0), _itPullPeer(_mapPeers.end()), _itPushPeer(_mapPeers.end()), _itFragmentsPeer(_mapPeers.end()), _lastFragmentMapId(0), _firstPullReceived(false), _poolBuffers(poolBuffers), 
	_stream(name), _streamKey(key), groupParameters(parameters), id(++GroupMediaCounter) {

	onPeerClose = [this](const string& peerId, UInt8 mask) {
		// unset push masks
		if (mask) {
			for (UInt8 i = 0; i < 8; i++) {
				if (mask & (1 << i)) {
					auto itPush = _mapPushMasks.find(1 << i);
					if (itPush != _mapPushMasks.end() && itPush->second.first == peerId)
						_mapPushMasks.erase(itPush);
				}
			}
		}
		removePeer(peerId);
	};
	onPlayPull = [this](PeerMedia* pPeer, UInt64 index) {

		auto itFragment = _fragments.find(index);
		if (itFragment == _fragments.end()) {
			DEBUG("GroupMedia ", id, " - Peer is asking for an unknown Fragment (", index, "), possibly deleted")
			return;
		}

		// Send fragment to peer (pull mode)
		pPeer->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.pBuffer.size(), itFragment->first, true);
	};
	onFragmentsMap = [this](UInt64 counter) {
		if (groupParameters->isPublisher)
			return false; // ignore the request

		// Record the idenfier for future pull requests
		if (_lastFragmentMapId < counter) {
			_mapPullTime2Fragment.emplace(Time::Now(), counter);
			_lastFragmentMapId = counter;
		}

		// Start push mode if not started
		if (_firstPushMode) {
			sendPushRequests();
			_firstPushMode = false;
		}
		return true;
	};
	onMedia = [this](bool reliable, AMF::ContentType type, UInt32 time, const UInt8* data, UInt32 size) {
		const UInt8* pos = data;
		const UInt8* end = data + size;
		UInt8 splitCounter = size / NETGROUP_MAX_PACKET_SIZE - ((size % NETGROUP_MAX_PACKET_SIZE) == 0);
		UInt8 marker = GroupStream::GROUP_MEDIA_DATA ;
		TRACE("GroupMedia ", id, " - Creating fragments ", _fragmentCounter + 1, " to ", _fragmentCounter + splitCounter, " - time : ", time)
		auto itFragment = _fragments.end();
		do {
			if (size > NETGROUP_MAX_PACKET_SIZE)
				marker = splitCounter == 0 ? GroupStream::GROUP_MEDIA_END : (pos == data ? GroupStream::GROUP_MEDIA_START : GroupStream::GROUP_MEDIA_NEXT);

			// Add the fragment to the map
			UInt32 fragmentSize = ((splitCounter > 0) ? NETGROUP_MAX_PACKET_SIZE : (end - pos));
			addFragment(itFragment, NULL, marker, ++_fragmentCounter, splitCounter, type, time, pos, fragmentSize);

			pos += splitCounter > 0 ? NETGROUP_MAX_PACKET_SIZE : (end - pos);
		} while (splitCounter-- > 0);

	};
	onFragment = [this](PeerMedia* pPeer, const string& peerId, UInt8 marker, UInt64 fragmentId, UInt8 splitedNumber, UInt8 mediaType, UInt32 time, PacketReader& packet, double lostRate) {

		// Pull fragment?
		auto itWaiting = _mapWaitingFragments.find(fragmentId);
		if (itWaiting != _mapWaitingFragments.end()) {
			TRACE("GroupMedia ", id, " - Waiting fragment ", fragmentId, " is arrived")
			_mapWaitingFragments.erase(itWaiting);
			if (!_firstPullReceived)
				_firstPullReceived = true;
		}
		// Push fragment
		else {
			UInt8 mask = 1 << (fragmentId % 8);
			if (pPeer->pushInMode & mask) {
				TRACE("GroupMedia ", id, " - Push In fragment received from ", peerId, " : ", fragmentId, " ; mask : ", Format<UInt8>("%.2x", mask))

				auto itPushMask = _mapPushMasks.lower_bound(mask);
				// first push with this mask?
				if (itPushMask == _mapPushMasks.end() || itPushMask->first != mask)
					_mapPushMasks.emplace_hint(itPushMask, piecewise_construct, forward_as_tuple(mask), forward_as_tuple(peerId.c_str(), fragmentId));
				else {
					if (itPushMask->second.first != peerId) {
						// Peer is faster?
						if (itPushMask->second.second < fragmentId) {
							TRACE("GroupMedia ", id, " - Push In - Updating the pusher, last peer was ", itPushMask->second.first)
							auto itOldPeer = _mapPeers.find(itPushMask->second.first);
							if (itOldPeer != _mapPeers.end())
								itOldPeer->second->sendPushMode(itOldPeer->second->pushInMode - mask);
							itPushMask->second.first = peerId.c_str();
						}
						else {
							TRACE("GroupMedia ", id, " - Push In - Tested pusher is slower than current one, resetting mask...")
							pPeer->sendPushMode(pPeer->pushInMode - mask);
						}
					}
					if (itPushMask->second.second < fragmentId)
						itPushMask->second.second = fragmentId; // update the last id received for this mask
				}
			}
			else
				DEBUG("GroupMedia ", id, " - Unexpected fragment received from ", peerId, " : ", fragmentId, " ; mask : ", Format<UInt8>("%.2x", mask))
		}

		auto itFragment = _fragments.lower_bound(fragmentId);
		if (itFragment != _fragments.end() && itFragment->first == fragmentId) {
			TRACE("GroupMedia ", id, " - Fragment ", fragmentId, " already received, ignored")
			return;
		}

		// Add the fragment to the map
		addFragment(itFragment, pPeer, marker, fragmentId, splitedNumber, mediaType, time, packet.current(), packet.available());

		// Push the fragment to the output file (if ordered)
		pushFragment(itFragment);
	};
}

GroupMedia::~GroupMedia() {
	TRACE("Closing the GroupMedia ", id)

	MAP_PEERS_INFO_ITERATOR_TYPE itPeer = _mapPeers.begin();
	while (itPeer != _mapPeers.end())
		removePeer(itPeer++);

	_fragments.clear();
	_mapTime2Fragment.clear();
}

void GroupMedia::addFragment(MAP_FRAGMENTS_ITERATOR& itFragment, PeerMedia* pPeer, UInt8 marker, UInt64 id, UInt8 splitedNumber, UInt8 mediaType, UInt32 time, const UInt8* data, UInt32 size) {
	UInt32 bufferSize = size + 1 + 5 * (marker == GroupStream::GROUP_MEDIA_START || marker == GroupStream::GROUP_MEDIA_DATA) + (splitedNumber > 0) + Util::Get7BitValueSize(id);
	itFragment = _fragments.emplace_hint(itFragment, piecewise_construct, forward_as_tuple(id), forward_as_tuple(_poolBuffers, data, size, bufferSize, time, (AMF::ContentType)mediaType,
		id, marker, splitedNumber));

	// Send fragment to peers (push mode)
	UInt8 nbPush = groupParameters->pushLimit + 1;
	for (auto it : _mapPeers) {
		if (it.second.get() != pPeer && it.second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.pBuffer.size(), id) && (--nbPush == 0)) {
			TRACE("GroupMedia ", id, " - Push limit (", groupParameters->pushLimit + 1, ") reached for fragment ", id, " (mask=", Format<UInt8>("%.2x", 1 << (id % 8)), ")")
			break;
		}
	}

	if ((marker == GroupStream::GROUP_MEDIA_DATA || marker == GroupStream::GROUP_MEDIA_START) && (_mapTime2Fragment.empty() || time > _mapTime2Fragment.rbegin()->first))
		_mapTime2Fragment[time] = id;
}

void GroupMedia::manage() {
	if (_mapPeers.empty())
		return;

	// Send the Fragments Map message
	UInt64 lastFragment(0);
	if (_lastFragmentsMap.isElapsed(groupParameters->availabilityUpdatePeriod) && (lastFragment = updateFragmentMap())) {

		// Send to all neighbors
		if (groupParameters->availabilitySendToAll) {
			for (auto it : _mapPeers) {
				it.second->sendFragmentsMap(lastFragment, _fragmentsMapBuffer.data(), _fragmentsMapBuffer.size());
			}
		} // Or just one peer at random
		else {	
			if ((_itFragmentsPeer == _mapPeers.end() && RTMFP::getRandomIt<MAP_PEERS_INFO_TYPE, MAP_PEERS_INFO_ITERATOR_TYPE>(_mapPeers, _itFragmentsPeer, [](const MAP_PEERS_INFO_ITERATOR_TYPE& it) { return true; })) 
					|| getNextPeer(_itFragmentsPeer, false, 0, 0))
				_itFragmentsPeer->second->sendFragmentsMap(lastFragment, _fragmentsMapBuffer.data(), _fragmentsMapBuffer.size());
		}
		_lastFragmentsMap.update();
	}

	// Send the Push requests
	if (!groupParameters->isPublisher && _lastPushUpdate.isElapsed(NETGROUP_PUSH_DELAY))
		sendPushRequests();

	// Send the Pull requests
	if (!groupParameters->isPublisher && _lastPullUpdate.isElapsed(NETGROUP_PULL_DELAY)) {

		sendPullRequests();
		_lastPullUpdate.update();
	}
}

void GroupMedia::addPeer(const string& peerId, shared_ptr<PeerMedia>& pPeer) {
	auto itPeer = _mapPeers.lower_bound(peerId);
	if (itPeer != _mapPeers.end() && itPeer->first == peerId)
		return;

	_mapPeers.emplace(peerId, pPeer);
	pPeer->OnPeerClose::subscribe(onPeerClose);
	pPeer->OnPlayPull::subscribe(onPlayPull);
	pPeer->OnFragmentsMap::subscribe(onFragmentsMap);
	pPeer->OnFragment::subscribe(onFragment);
	DEBUG("GroupMedia ", id, " - Adding peer ", peerId, " (", _mapPeers.size(), " peers)")

	// Send the group media & fragments map if not already sent
	sendGroupMedia(pPeer);
}

void GroupMedia::sendGroupMedia(shared_ptr<PeerMedia>& pPeer) {
	if (pPeer->groupMediaSent)
		return;

	pPeer->sendGroupMedia(_stream, _streamKey, groupParameters.get());
	UInt64 lastFragment = updateFragmentMap();
	if (!pPeer->sendFragmentsMap(lastFragment, _fragmentsMapBuffer.data(), _fragmentsMapBuffer.size()))
		pPeer->flushReportWriter();
}

bool GroupMedia::getNextPeer(MAP_PEERS_INFO_ITERATOR_TYPE& itPeer, bool ascending, UInt64 idFragment, UInt8 mask) {
	if (!_mapPeers.empty()) {

		// To go faster when there is only one peer
		if (_mapPeers.size() == 1) {
			itPeer = _mapPeers.begin();
			if (itPeer != _mapPeers.end() && (!idFragment || itPeer->second->hasFragment(idFragment)) && (!mask || !(itPeer->second->pushInMode & mask)))
				return true;
		}
		else {

			auto itBegin = itPeer;
			do {
				if (ascending)
					(itPeer == _mapPeers.end()) ? itPeer = _mapPeers.begin() : ++itPeer;
				else // descending
					(itPeer == _mapPeers.begin()) ? itPeer = _mapPeers.end() : --itPeer;

				// Peer match? Exiting
				if (itPeer != _mapPeers.end() && (!idFragment || itPeer->second->hasFragment(idFragment)) && (!mask || !(itPeer->second->pushInMode & mask)))
					return true;
			}
			// loop until finding a peer available
			while (itPeer != itBegin);
		}
	}

	return false;
}

void GroupMedia::eraseOldFragments() {
	if (_fragments.empty())
		return;

	UInt32 end = _fragments.rbegin()->second.time;
	UInt32 time2Keep = end - (groupParameters->windowDuration + groupParameters->relayMargin);
	auto itTime = _mapTime2Fragment.lower_bound(time2Keep);

	// To not delete more than the window duration
	if (itTime != _mapTime2Fragment.end() && time2Keep > itTime->first)
		--itTime;
		
	// Ignore if no fragment found or if it is the first reference
	if (itTime == _mapTime2Fragment.end() || itTime == _mapTime2Fragment.begin())
		return;

	auto itFragment = _fragments.find(itTime->second);
	if (itFragment == _fragments.end()) {
		ERROR("GroupMedia ", id, " - Unable to find the fragment ", itTime->second, " for cleaning buffer") // implementation error
		return;
	}

	// Get the first fragment before the itTime reference
	--itFragment;
	if (_fragmentCounter < itFragment->first) {
		WARN("GroupMedia ", id, " - Deleting unread fragments to keep the window duration... (", itFragment->first - _fragmentCounter, " fragments ignored)")
		_fragmentCounter = itFragment->first;
	}

	DEBUG("GroupMedia ", id, " - Deletion of fragments ", _fragments.begin()->first, " (~", _mapTime2Fragment.begin()->first, ") to ",
		itFragment->first, " (~", itTime->first, ") - current time : ", end)
	_fragments.erase(_fragments.begin(), itFragment);
	_mapTime2Fragment.erase(_mapTime2Fragment.begin(), itTime);

	// Delete the old waiting fragments
	auto itWait = _mapWaitingFragments.lower_bound(itFragment->first);
	if (!_mapWaitingFragments.empty() && _mapWaitingFragments.begin()->first < itFragment->first) {
		WARN("GroupMedia ", id, " - Deletion of waiting fragments ", _mapWaitingFragments.begin()->first, " to ", (itWait == _mapWaitingFragments.end())? _mapWaitingFragments.rbegin()->first : itWait->first)
		_mapWaitingFragments.erase(_mapWaitingFragments.begin(), itWait);
	}
	if (_currentPullFragment < itFragment->first)
		_currentPullFragment = itFragment->first; // move the current pull fragment to the 1st fragment

	// Try to push again the last fragments
	auto itLast = _fragments.find(_fragmentCounter + 1);
	if (itLast != _fragments.end())
		pushFragment(itLast);
}

UInt64 GroupMedia::updateFragmentMap() {
	if (_fragments.empty())
		return 0;

	// First we erase old fragments
	eraseOldFragments();

	// Generate the report message
	UInt64 firstFragment = _fragments.begin()->first;
	UInt64 lastFragment = _fragments.rbegin()->first;
	UInt64 nbFragments = lastFragment - firstFragment; // number of fragments - the first one
	_fragmentsMapBuffer.resize((UInt32)((nbFragments / 8) + ((nbFragments % 8) > 0)) + Util::Get7BitValueSize(lastFragment) + 1, false);
	BinaryWriter writer(BIN _fragmentsMapBuffer.data(), _fragmentsMapBuffer.size());
	writer.write8(GroupStream::GROUP_FRAGMENTS_MAP).write7BitLongValue(lastFragment);

	// If there is only one fragment we just write its counter
	if (!nbFragments)
		return lastFragment;

	if (groupParameters->isPublisher) { // Publisher : We have all fragments, faster treatment
		
		while (nbFragments > 8) {
			writer.write8(0xFF);
			nbFragments -= 8;
		}
		UInt8 lastByte = 1;
		while (--nbFragments > 0)
			lastByte = (lastByte << 1) + 1;
		writer.write8(lastByte);
	}
	else {
		// Loop on each bit
		for (UInt64 index = lastFragment - 1; index >= firstFragment && index >= 8; index -= 8) {

			UInt8 currentByte = 0;
			for (UInt8 fragment = 0; fragment < 8 && (index-fragment) >= firstFragment; fragment++) {
				if (_fragments.find(index - fragment) != _fragments.end())
					currentByte += (1 << fragment);
			}
			writer.write8(currentByte);
		}
	}

	return lastFragment;
}

bool GroupMedia::pushFragment(map<UInt64, MediaPacket>::iterator& itFragment) {
	if (itFragment == _fragments.end() || !_firstPullReceived)
		return false;

	// Stand alone fragment (special case : sometime Flash send media END without splitted fragments)
	if (itFragment->second.marker == GroupStream::GROUP_MEDIA_DATA || (itFragment->second.marker == GroupStream::GROUP_MEDIA_END && itFragment->first == _fragmentCounter + 1)) {
		// Is it the next fragment?
		if (_fragmentCounter == 0 || itFragment->first == _fragmentCounter + 1) {
			_fragmentCounter = itFragment->first;

			TRACE("GroupMedia ", id, " - Pushing Media Fragment ", itFragment->first)
			if (itFragment->second.type == AMF::AUDIO || itFragment->second.type == AMF::VIDEO)
				OnGroupPacket::raise(itFragment->second.time, itFragment->second.payload, itFragment->second.payloadSize(), 0, itFragment->second.type == AMF::AUDIO);

			return pushFragment(++itFragment); // Go to next fragment
		}
	}
	// Splitted packet
	else  {
		if (_fragmentCounter == 0) {
			// Delete first splitted fragments
			if (itFragment->second.marker != GroupStream::GROUP_MEDIA_START) {
				TRACE("GroupMedia ", id, " - Ignoring splitted fragment ", itFragment->first, ", we are waiting for a starting fragment")
				_fragments.erase(itFragment);
				return false;
			}
			else {
				TRACE("GroupMedia ", id, " - First fragment is a Start Media Fragment")
				_fragmentCounter = itFragment->first-1; // -1 to be catched by the next fragment condition 
			}
		}

		// Search the start fragment
		auto itStart = itFragment;
		while (itStart->second.marker != GroupStream::GROUP_MEDIA_START) {
			itStart = _fragments.find(itStart->first - 1);
			if (itStart == _fragments.end())
				return false; // ignore these fragments if there is a hole
		}
		
		// Check if all splitted fragments are present
		UInt8 nbFragments = itStart->second.splittedId+1;
		UInt32 payloadSize = itStart->second.payloadSize();
		auto itEnd = itStart;
		for (int i = 1; i < nbFragments; ++i) {
			itEnd = _fragments.find(itStart->first + i);
			if (itEnd == _fragments.end())
				return false; // ignore these fragments if there is a hole

			payloadSize += itEnd->second.payloadSize();
		}

		// Is it the next fragment?
		if (itStart->first == _fragmentCounter + 1) {
			_fragmentCounter = itEnd->first;

			// Buffer the fragments and write to file if audio/video
			if (itStart->second.type == AMF::AUDIO || itStart->second.type == AMF::VIDEO) {
				Buffer	payload(payloadSize);
				BinaryWriter writer(payload.data(), payloadSize);
				auto	itCurrent = itStart;

				do {
					writer.write(itCurrent->second.payload, itCurrent->second.payloadSize());
				} while (itCurrent++ != itEnd);

				TRACE("GroupMedia ", id, " - Pushing splitted packet ", itStart->first, " - ", nbFragments, " fragments for a total size of ", payloadSize)
				OnGroupPacket::raise(itStart->second.time, payload.data(), payloadSize, 0, itStart->second.type == AMF::AUDIO);
			}

			return pushFragment(++itEnd);
		}
	}

	return false;
}

void GroupMedia::sendPushRequests() {
	if (!_mapPeers.empty()) {

		// First bit mask is random, next are incremental
		_currentPushMask = (!_currentPushMask) ? 1 << (Util::Random<UInt8>() % 8) : ((_currentPushMask == 0x80) ? 1 : _currentPushMask << 1);
		TRACE("GroupMedia ", id, " - Push In - Current mask is ", Format<UInt8>("%.2x", _currentPushMask))

		// Get the next peer & send the push request
		if ((_itPushPeer == _mapPeers.end() && RTMFP::getRandomIt<MAP_PEERS_INFO_TYPE, MAP_PEERS_INFO_ITERATOR_TYPE>(_mapPeers, _itPushPeer, [this](const MAP_PEERS_INFO_ITERATOR_TYPE& it) { return !(it->second->pushInMode & _currentPushMask); }))
				|| getNextPeer(_itPushPeer, false, 0, _currentPushMask))
			_itPushPeer->second->sendPushMode(_itPushPeer->second->pushInMode | _currentPushMask);
		else
			TRACE("GroupMedia ", id, " - Push In - No new peer available for mask ", Format<UInt8>("%.2x", _currentPushMask))
	}

	_lastPushUpdate.update();
}

void GroupMedia::sendPullRequests() {
	if (_mapPullTime2Fragment.empty()) // not started yet
		return;

	Int64 timeNow(Time::Now());
	Int64 timeMax = timeNow - groupParameters->fetchPeriod;
	auto maxFragment = _mapPullTime2Fragment.lower_bound(timeMax);
	if (maxFragment == _mapPullTime2Fragment.begin() || maxFragment == _mapPullTime2Fragment.end()) {
		if ((timeNow - _mapPullTime2Fragment.begin()->first) > groupParameters->fetchPeriod)
			DEBUG("GroupMedia ", id, " - sendPullRequests - No Fragments map received since Fectch period (", groupParameters->fetchPeriod, "ms), possible network issue")
		// else we are waiting for fetch period before starting pull requests
		return;
	}
	UInt64 lastFragment = (--maxFragment)->second; // get the first fragment < the fetch period
	
	// The first pull request get the latest known fragments
	if (!_currentPullFragment) {
		_currentPullFragment = (lastFragment > 1)? lastFragment - 1 : 1;
		auto itRandom1 = _mapPeers.begin();
		_itPullPeer = _mapPeers.begin();
		if (RTMFP::getRandomIt<MAP_PEERS_INFO_TYPE, MAP_PEERS_INFO_ITERATOR_TYPE>(_mapPeers, itRandom1, [this](const MAP_PEERS_INFO_ITERATOR_TYPE& it) { return it->second->hasFragment(_currentPullFragment); })) {
			TRACE("GroupMedia ", id, " - sendPullRequests - first fragment found : ", _currentPullFragment)
			if (_fragments.find(_currentPullFragment) == _fragments.end()) { // ignoring if already received
				itRandom1->second->sendPull(_currentPullFragment);
				_mapWaitingFragments.emplace(piecewise_construct, forward_as_tuple(_currentPullFragment), forward_as_tuple(itRandom1->first.c_str()));
			}
			else
				_firstPullReceived = true;
		} else
			TRACE("GroupMedia ", id, " - sendPullRequests - Unable to find the first fragment (", _currentPullFragment, ")")
		if (RTMFP::getRandomIt<MAP_PEERS_INFO_TYPE, MAP_PEERS_INFO_ITERATOR_TYPE>(_mapPeers, _itPullPeer, [this](const MAP_PEERS_INFO_ITERATOR_TYPE& it) { return it->second->hasFragment(_currentPullFragment + 1); })) {
			TRACE("GroupMedia ", id, " - sendPullRequests - second fragment found : ", _currentPullFragment + 1)
			if (_fragments.find(++_currentPullFragment) == _fragments.end()) { // ignoring if already received
				_itPullPeer->second->sendPull(_currentPullFragment);
				_mapWaitingFragments.emplace(piecewise_construct, forward_as_tuple(_currentPullFragment), forward_as_tuple(_itPullPeer->first.c_str()));
			}
			else
				_firstPullReceived = true;
			return;
		}
		TRACE("GroupMedia ", id, " - sendPullRequests - Unable to find the second fragment (", _currentPullFragment + 1, ")")
		_currentPullFragment = 0; // no pullers found
		return;
	}

	// Loop on older fragments to send back the requests
	timeMax -= groupParameters->fetchPeriod;
	maxFragment = _mapPullTime2Fragment.lower_bound(timeMax);
	if (maxFragment != _mapPullTime2Fragment.begin() && maxFragment != _mapPullTime2Fragment.end()) {
		UInt64 lastOldFragment = (--maxFragment)->second; // get the first fragment < the fetch period * 2
		for (auto itPull = _mapWaitingFragments.begin(); itPull != _mapWaitingFragments.end() && itPull->first <= lastOldFragment; itPull++) {

			// Fetch period elapsed? => blacklist the peer and send back the request to another peer
			if (itPull->second.time.isElapsed(groupParameters->fetchPeriod)) {

				DEBUG("GroupMedia ", id, " - sendPullRequests - ", groupParameters->fetchPeriod, "ms without receiving fragment ", itPull->first, ", blacklisting peer ", itPull->second.peerId)
				auto itPeer = _mapPeers.find(itPull->second.peerId);
				if (itPeer != _mapPeers.end())
					itPeer->second->addPullBlacklist(itPull->first);
				
				if (sendPullToNextPeer(itPull->first)) {
					itPull->second.peerId = _itPullPeer->first.c_str();
					itPull->second.time.update();
				}
			}
		}
	}

	// Find the holes and send pull requests
	for (; _currentPullFragment < lastFragment; _currentPullFragment++) {

		if (_fragments.find(_currentPullFragment + 1) == _fragments.end() && !sendPullToNextPeer(_currentPullFragment + 1))
			break; // we wait for the fragment to be available
	}

	TRACE("GroupMedia ", id, " - sendPullRequests - Pull requests done : ", _mapWaitingFragments.size(), " waiting fragments (current : ", _currentPullFragment, "; last Fragment : ", lastFragment, ")")
}

bool GroupMedia::sendPullToNextPeer(UInt64 idFragment) {

	if (!getNextPeer(_itPullPeer, true, idFragment, 0)) {
		DEBUG("GroupMedia ", id, " - sendPullRequests - No peer found for fragment ", idFragment)
		return false;
	}
	
	_itPullPeer->second->sendPull(idFragment);
	_mapWaitingFragments.emplace(piecewise_construct, forward_as_tuple(idFragment), forward_as_tuple(_itPullPeer->first.c_str()));
	return true;
}

void GroupMedia::removePeer(const string& peerId) {
	
	auto itPeer = _mapPeers.find(peerId);
	if (itPeer != _mapPeers.end())
		removePeer(itPeer);
}

void GroupMedia::removePeer(MAP_PEERS_INFO_ITERATOR_TYPE itPeer) {

	DEBUG("GroupMedia ", id, " - Removing peer ", itPeer->first, " (", _mapPeers.size()," peers)")
	itPeer->second->OnPeerClose::unsubscribe(onPeerClose);
	itPeer->second->OnPlayPull::unsubscribe(onPlayPull);
	itPeer->second->OnFragmentsMap::unsubscribe(onFragmentsMap);
	itPeer->second->OnFragment::unsubscribe(onFragment);

	// If it is a current peer => increment
	if (itPeer == _itPullPeer && getNextPeer(_itPullPeer, true, 0, 0) && itPeer == _itPullPeer)
		_itPullPeer = _mapPeers.end(); // to avoid bad pointer
	if (itPeer == _itPushPeer && getNextPeer(_itPushPeer, false, 0, 0) && itPeer == _itPushPeer)
		_itPushPeer = _mapPeers.end(); // to avoid bad pointer
	if (itPeer == _itFragmentsPeer && getNextPeer(_itFragmentsPeer, false, 0, 0) && itPeer == _itFragmentsPeer)
		_itFragmentsPeer = _mapPeers.end(); // to avoid bad pointer
	_mapPeers.erase(itPeer);
}

void GroupMedia::callFunction(const char* function, int nbArgs, const char** args) {
	if (!groupParameters->isPublisher) // only publisher can create fragments
		return;

	AMFWriter writer(_poolBuffers);
	writer.amf0 = true;
	writer.packet.write8(0);
	writer.writeString(function, strlen(function));
	for (int i = 0; i < nbArgs; i++) {
		if (args[i])
			writer.writeString(args[i], strlen(args[i]));
	}

	UInt32 currentTime = (_fragments.empty())? 0 : _fragments.rbegin()->second.time;

	// Create and send the fragment
	TRACE("Creating fragment for function ", function, "...")
	onMedia(true, AMF::DATA_AMF3, currentTime, writer.packet.data(), writer.packet.size());
}
