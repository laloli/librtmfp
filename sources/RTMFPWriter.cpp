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

#include "RTMFPWriter.h"
#include "Mona/Util.h"
#include "Mona/Logs.h"
#include "GroupStream.h"
#include "librtmfp.h"

using namespace std;
using namespace Mona;

RTMFPWriter::RTMFPWriter(State state,const string& signature, BandWriter& band, shared_ptr<RTMFPWriter>& pThis, UInt64 idFlow) : FlashWriter(state,band.poolBuffers()), id(0), _band(band),
	_stage(0), _stageAck(0), flowId(idFlow), signature(signature), _repeatable(0), _lostCount(0), _ackCount(0) {

	pThis.reset(this);
	_band.initWriter(pThis);
	if (signature.empty())
		open();
}

RTMFPWriter::RTMFPWriter(State state,const string& signature, BandWriter& band, UInt64 idFlow) : FlashWriter(state,band.poolBuffers()), id(0), _band(band),
	_stage(0), _stageAck(0), flowId(idFlow), signature(signature), _repeatable(0), _lostCount(0), _ackCount(0) {

	shared_ptr<RTMFPWriter> pThis(this);
	_band.initWriter(pThis);
	if (signature.empty())
		open();
}

RTMFPWriter::RTMFPWriter(RTMFPWriter& writer) : FlashWriter(writer), _band(writer._band),
	_repeatable(writer._repeatable), _stage(writer._stage), _stageAck(writer._stageAck),
	_ackCount(writer._ackCount), _lostCount(writer._lostCount), flowId(writer.flowId), signature(writer.signature), id(writer.id) {
	reliable = true;
	close();
}

RTMFPWriter::~RTMFPWriter() {
	FlashWriter::close();
	abort();
}

void RTMFPWriter::abort() {

	// delete messages
	RTMFPMessage* pMessage;
	while(!_messages.empty()) {
		pMessage = _messages.front();
		_lostCount += pMessage->fragments.size();
		delete pMessage;
		_messages.pop_front();
	}
	while(!_messagesSent.empty()) {
		pMessage = _messagesSent.front();
		_lostCount += pMessage->fragments.size();
		if(pMessage->repeatable)
			--_repeatable;
		delete pMessage;
		_messagesSent.pop_front();
	}
	if(_stage>0) {
		createMessage(); // Send a MESSAGE_ABANDONMENT just in the case where the receiver has been created
		flush(false);
		_trigger.stop();
	}
}

void RTMFPWriter::clear() {

	for (RTMFPMessage* pMessage : _messages)
		delete pMessage;
	_messages.clear();
	FlashWriter::clear();
}

void RTMFPWriter::close(Int32 code) {
	if(state()==CLOSED)
		return;
	if(_stage>0 || _messages.size()>0)
		createMessage(); // Send a MESSAGE_END just in the case where the receiver has been created (or will be created)
	FlashWriter::close(code); 
	_closeTime.update();
}

bool RTMFPWriter::acknowledgment(Exception& ex, PacketReader& packet) {

	UInt64 bufferSize = packet.read7BitLongValue(); // TODO use this value in reliability mechanism?
	
	if(bufferSize==0) {
		// In fact here, we should send a 0x18 message (with id flow),
		// but it can create a loop... We prefer the following behavior
		WARN("Closing writer ", id, ", negative acknowledgment");
		close();
		return !ex;
	}

	UInt64 stageAckPrec = _stageAck;
	UInt64 stageReaden = packet.read7BitLongValue();
	UInt64 stage = _stageAck+1;

	if(stageReaden>_stage) {
		ERROR("Acknowledgment received ",stageReaden," superior than the current sending stage ",_stage," on writer ",id);
		_stageAck = _stage;
	} else if(stageReaden<=_stageAck) {
		// already acked
		if(packet.available()==0)
			DEBUG("Acknowledgment ",stageReaden," obsolete on writer ",id);
	} else
		_stageAck = stageReaden;

	UInt64 maxStageRecv = stageReaden;
	UInt32 pos=packet.position();

	while(packet.available()>0)
		maxStageRecv += packet.read7BitLongValue()+packet.read7BitLongValue()+2;
	if(pos != packet.position()) {
		// TRACE(stageReaden,"..x"Util::FormatHex(reader.current(),reader.available()));
		packet.reset(pos);
	}

	UInt64 lostCount = 0;
	UInt64 lostStage = 0;
	bool repeated = false;
	bool header = true;
	bool stop=false;

	auto it=_messagesSent.begin();
	while(!stop && it!=_messagesSent.end()) {
		RTMFPMessage& message(**it);

		if(message.fragments.empty()) {
			CRITIC("RTMFPMessage ",(stage+1)," is bad formatted on flowWriter ",id);
			++it;
			continue;
		}

		map<UInt32,UInt64>::iterator itFrag=message.fragments.begin();
		while(message.fragments.end()!=itFrag) {
			
			// ACK
			if(_stageAck>=stage) {
				message.fragments.erase(message.fragments.begin());
				itFrag=message.fragments.begin();
				++_ackCount;
				++stage;
				continue;
			}

			// Read lost informations
			while(!stop) {
				if(lostCount==0) {
					if(packet.available()>0) {
						lostCount = packet.read7BitLongValue()+1;
						lostStage = stageReaden+1;
						stageReaden = lostStage+lostCount+packet.read7BitLongValue();
					} else {
						stop=true;
						break;
					}
				}
				// check the range
				if(lostStage>_stage) {
					// Not yet sent
					ERROR("Lost information received ",lostStage," have not been yet sent on writer ",id);
					stop=true;
				} else if(lostStage<=_stageAck) {
					// already acked
					--lostCount;
					++lostStage;
					continue;
				}
				break;
			}
			if(stop)
				break;
			
			// lostStage > 0 and lostCount > 0

			if(lostStage!=stage) {
				if(repeated) {
					++stage;
					++itFrag;
					header=true;
				} else // No repeated, it means that past lost packet was not repeatable, we can ack this intermediate received sequence
					_stageAck = stage;
				continue;
			}

			/// Repeat message asked!
			if(!message.repeatable) {
				if(repeated) {
					++itFrag;
					++stage;
					header=true;
				} else {
					INFO("RTMFPWriter ",id," : message ",stage," lost");
					--_ackCount;
					++_lostCount;
					_stageAck = stage;
				}
				--lostCount;
				++lostStage;
				continue;
			}

			repeated = true;
			// Don't repeat before that the receiver receives the itFrag->second sending stage
			if(itFrag->second >= maxStageRecv) {
				++stage;
				header=true;
				--lostCount;
				++lostStage;
				++itFrag;
				continue;
			}

			// Repeat message

			DEBUG("RTMFPWriter ",id," : stage ",stage," repeated");
			UInt32 fragment(itFrag->first);
			itFrag->second = _stage; // Save actual stage sending to wait that the receiver gets it before to retry
			UInt32 contentSize = message.size() - fragment; // available
			++itFrag;

			// Compute flags
			UInt8 flags = 0;
			if(fragment>0)
				flags |= MESSAGE_WITH_BEFOREPART; // fragmented
			if(itFrag!=message.fragments.end()) {
				flags |= MESSAGE_WITH_AFTERPART;
				contentSize = itFrag->first - fragment;
			}

			UInt32 size = contentSize+4;
			UInt32 availableToWrite(_band.availableToWrite());
			if(!header && size>availableToWrite) {
				_band.flush();
				header=true;
			}

			if(header)
				size+=headerSize(stage);

			if(size>availableToWrite)
				_band.flush();

			// Write packet
			size-=3;  // type + timestamp removed, before the "writeMessage"
			packMessage(_band.writeMessage(header ? 0x10 : 0x11,(UInt16)size),stage,flags,header,message,fragment,contentSize);
			header=false;
			--lostCount;
			++lostStage;
			++stage;
		}

		if(message.fragments.empty()) {
			if(message.repeatable)
				--_repeatable;
			if(_ackCount || _lostCount) {
				//TODO : _qos.add(_lostCount / (_lostCount + _ackCount));
				_ackCount=_lostCount=0;
			}
			
			delete *it;
			it=_messagesSent.erase(it);
		} else
			++it;
	
	}

	if(lostCount>0 && packet.available()>0)
		ERROR("Some lost information received have not been yet sent on writer ",id);


	// rest messages repeatable?
	if(_repeatable==0)
		_trigger.stop();
	else if(_stageAck>stageAckPrec || repeated)
		_trigger.reset();
	return true;
}

void RTMFPWriter::manage(Exception& ex) {
	if(!consumed() && !_band.failed()) {
		
		// if some acknowlegment has not been received we send the messages back (8 times, with progressive occurences)
		if (_trigger.raise(ex)) {
			TRACE("Sending back repeatable messages (cycle : ", _trigger.cycle(), ")")
			raiseMessage();
		}
		// When the peer/server doesn't send acknowledgment since a while we close the writer
		else if (ex) {
			WARN("Closing writer ", id, ", can't deliver its data : ", ex.error());
			close();
			return;
		}
	}
	/*if(critical && state()==CLOSED) {
		ex.set(Exception::NETWORK, "Main flow writer closed, session is closing");
		return;
	}*/
	flush();
}

UInt32 RTMFPWriter::headerSize(UInt64 stage) { // max size header = 50
	UInt32 size= Util::Get7BitValueSize(id);
	size+= Util::Get7BitValueSize(stage);
	if(_stageAck>stage)
		CRITIC("stageAck ",_stageAck," superior to stage ",stage," on writer ",id);
	size+= Util::Get7BitValueSize(stage-_stageAck);
	size+= _stageAck>0 ? 0 : (signature.size()+((flowId==0 || id<=2)?2:(4+Util::Get7BitValueSize(flowId)))); // TODO: check the condition id<=2 (see next TODO below)
	return size;
}


void RTMFPWriter::packMessage(BinaryWriter& writer,UInt64 stage,UInt8 flags,bool header,const RTMFPMessage& message, UInt32 offset, UInt16 size) {
	if(_stageAck==0 && header)
		flags |= MESSAGE_HEADER;
	if(size==0)
		flags |= MESSAGE_ABANDONMENT;
	if(state()==CLOSED && _messages.size()==1) // On LAST message
		flags |= MESSAGE_END;

	// TRACE("RTMFPWriter ",id," stage ",stage);

	writer.write8(flags);

	if(header) {
		writer.write7BitLongValue(id);
		writer.write7BitLongValue(stage);
		writer.write7BitLongValue(stage-_stageAck);

		// signature
		if(_stageAck==0) {
			writer.write8((UInt8)signature.size()).write(signature);
			// Send flowId for a new flow (not for writer 2 => AMS support)
			if(id>2) {
				writer.write8(1+Util::Get7BitValueSize(flowId)); // following size
				writer.write8(0x0a); // Unknown!
				writer.write7BitLongValue(flowId);
			}
			writer.write8(0); // marker of end for this part
		}
	}

	if (size == 0)
		return;

	if (offset < message.frontSize()) {
		UInt8 count = message.frontSize()-offset;
		if (size<count)
			count = (UInt8)size;
		writer.write(message.front()+offset,count);
		size -= count;
		if (size == 0)
			return;
		offset += count;
	}

	writer.write(message.body()+offset-message.frontSize(), size);
}

void RTMFPWriter::raiseMessage() {
	bool header = true;
	bool stop = true;
	bool sent = false;
	UInt64 stage = _stageAck+1;

	for(RTMFPMessage* pMessage : _messagesSent) {
		RTMFPMessage& message(*pMessage);
		
		if(message.fragments.empty())
			break;

		// not repeat unbuffered messages
		if(!message.repeatable) {
			stage += message.fragments.size();
			header = true;
			continue;
		}
		
		/// HERE -> message repeatable AND already flushed one time!

		if(stop) {
			_band.flush(); // To repeat message, before we must send precedent waiting mesages
			stop = false;
		}

		map<UInt32,UInt64>::const_iterator itFrag=message.fragments.begin();
		UInt32 available = message.size()-itFrag->first;
	
		while(itFrag!=message.fragments.end()) {
			UInt32 contentSize = available;
			UInt32 fragment(itFrag->first);
			++itFrag;

			// Compute flags
			UInt8 flags = 0;
			if(fragment>0)
				flags |= MESSAGE_WITH_BEFOREPART; // fragmented
			if(itFrag!=message.fragments.end()) {
				flags |= MESSAGE_WITH_AFTERPART;
				contentSize = itFrag->first - fragment;
			}

			UInt32 size = contentSize+4;

			if(header)
				size+=headerSize(stage);

			// Actual sending packet is enough large? Here we send just one packet!
			if(size>_band.availableToWrite()) {
				if(!sent)
					ERROR("Raise messages on writer ",id," without sending!");
				DEBUG("Raise message on writer ",id," finishs on stage ",stage);
				return;
			}
			sent=true;

			// Write packet
			size-=3;  // type + timestamp removed, before the "writeMessage"
			packMessage(_band.writeMessage(header ? 0x10 : 0x11,(UInt16)size),stage++,flags,header,message,fragment,contentSize);
			available -= contentSize;
			header=false;
		}
	}

	if(stop)
		_trigger.stop();
}

bool RTMFPWriter::flush(bool full) {

	if(_messagesSent.size()>100)
		TRACE("Buffering become high : _messagesSent.size()=",_messagesSent.size());

	if(state()==OPENING) {
		ERROR("Violation policy, impossible to flush data on a opening writer");
		return false;
	}

	bool hasSent(false);

	// flush
	bool header = !_band.canWriteFollowing(*this);

	while(!_messages.empty()) {
		hasSent = true;

		RTMFPMessage& message(*_messages.front());

		if(message.repeatable) {
			++_repeatable;
			_trigger.start();
		}

		UInt32 fragments= 0;
		UInt32 available = message.size();
	
		do {

			++_stage;

			// Actual sending packet is enough large?
			UInt32 contentSize = _band.availableToWrite();
			UInt32 headerSize = (header && contentSize<62) ? this->headerSize(_stage) : 0; // calculate only if need!
			if(contentSize<(headerSize+12)) { // 12 to have a size minimum of fragmentation
				_band.flush(); // send packet (and without time echo)
				header=true;
			}

			contentSize = available;
			UInt32 size = contentSize+4;
			
			if(header)
				size+= headerSize>0 ? headerSize : this->headerSize(_stage);

			// Compute flags
			UInt8 flags = 0;
			if(fragments>0)
				flags |= MESSAGE_WITH_BEFOREPART;

			bool head = header;
			UInt32 availableToWrite(_band.availableToWrite());
			if(size>availableToWrite) {
				// the packet will change! The message will be fragmented.
				flags |= MESSAGE_WITH_AFTERPART;
				contentSize = availableToWrite-(size-contentSize);
				size=availableToWrite;
				header=true;
			} else
				header=false; // the packet stays the same!

			// Write packet
			size-=3; // type + timestamp removed, before the "writeMessage"
			packMessage(_band.writeMessage(head ? 0x10 : 0x11,(UInt16)size,this),_stage,flags,head,message,fragments,contentSize);
			//DEBUG("RTMFPWriter ", id, " : sending message ", _stage);
			
			message.fragments[fragments] = _stage;
			available -= contentSize;
			fragments += contentSize;

		} while(available>0);

		//TODO : _qos.add(message.size(),_band.ping());
		_messagesSent.emplace_back(&message);
		_messages.pop_front();
	}

	if (full)
		_band.flush();
	return hasSent;
}

RTMFPMessageBuffered& RTMFPWriter::createMessage() {
	if (state() == CLOSED || _band.failed()) {
		static RTMFPMessageBuffered MessageNull;
		return MessageNull;
	}
	RTMFPMessageBuffered* pMessage = new RTMFPMessageBuffered(_band.poolBuffers(),reliable);
	_messages.emplace_back(pMessage);
	return *pMessage;
}

AMFWriter& RTMFPWriter::write(AMF::ContentType type,UInt32 time,const UInt8* data, UInt32 size) {
	if (type < AMF::AUDIO || type > AMF::VIDEO)
		time = 0; // Because it can "dropped" the packet otherwise (like if the Writer was not reliable!)
	if(data && !reliable && state()==OPENED && !_band.failed()) {
		_messages.emplace_back(new RTMFPMessageUnbuffered(type,time,data,size));
		flush(false);
        return AMFWriter::Null;
	}
	AMFWriter& amf = createMessage().writer();
	BinaryWriter& binary(amf.packet);
	binary.write8(type);
	if (type == AMF::INVOCATION_AMF3) // Added for Play request in P2P, TODO: see if it is really needed
		binary.write8(0);
	binary.write32(time);
	if(type==AMF::DATA_AMF3)
		binary.write8(0);
	if(data)
		binary.write(data,size);
	return amf;
}

void RTMFPWriter::writeGroupConnect(const string& netGroup) {
	string tmp(netGroup.c_str()); // To avoid memory sharing we use c_str() (copy-on-write implementation on linux)
	createMessage().writer().packet.write8(GroupStream::GROUP_INIT).write16(0x2115).write(Util::UnformatHex(tmp)); // binary string
}

void RTMFPWriter::writePeerGroup(const string& netGroup, const UInt8* key, const char* rawId) {

	PacketWriter& writer = createMessage().writer().packet;
	writer.write8(GroupStream::GROUP_INIT).write16(0x4100).write(netGroup); // hexa format
	writer.write16(0x2101).write(key, Crypto::HMAC::SIZE);
	writer.write16(0x2303).write(rawId, PEER_ID_SIZE+2); // binary format
}

void RTMFPWriter::writeGroupBegin() {
	createMessage().writer().packet.write8(AMF::ABORT);
	createMessage().writer().packet.write8(GroupStream::GROUP_BEGIN);
}

void RTMFPWriter::writeGroupMedia(const std::string& streamName, const UInt8* data, UInt32 size, RTMFPGroupConfig* groupConfig) {

	PacketWriter& writer = createMessage().writer().packet;
	writer.write8(GroupStream::GROUP_MEDIA_INFOS).write7BitEncoded(streamName.size() + 1).write8(0).write(streamName);
	writer.write(data, size);
	writer.write("\x01\x02");
	if (groupConfig->availabilitySendToAll)
		writer.write("\x01\x06");
	writer.write8(1 + Util::Get7BitValueSize(UInt32(groupConfig->windowDuration))).write8('\x03').write7BitLongValue(groupConfig->windowDuration);
	writer.write("\x04\x04\x92\xA7\x60"); // Object encoding?
	writer.write8(1 + Util::Get7BitValueSize(groupConfig->availabilityUpdatePeriod)).write8('\x05').write7BitLongValue(groupConfig->availabilityUpdatePeriod);
	writer.write8(1 + Util::Get7BitValueSize(UInt32(groupConfig->fetchPeriod))).write8('\x07').write7BitLongValue(groupConfig->fetchPeriod);
}

void RTMFPWriter::writeGroupPlay(UInt8 mode) {
	PacketWriter& writer = createMessage().writer().packet;
	writer.write8(GroupStream::GROUP_PLAY_PUSH).write8(mode);
}

void RTMFPWriter::writeGroupPull(UInt64 index) {
	PacketWriter& writer = createMessage().writer().packet;
	writer.write8(GroupStream::GROUP_PLAY_PULL).write7BitLongValue(index);
}

void RTMFPWriter::writeRaw(const UInt8* data,UInt32 size) {
	if(reliable || state()==OPENING) {
		createMessage().writer().packet.write(data,size);
		return;
	}
	if(state()==CLOSED || _band.failed())
		return;
	_messages.emplace_back(new RTMFPMessageUnbuffered(data, size));
	flush(false);
}

/*
void RTMFPWriter::sendGroupCloseStream(UInt8 type, UInt64 fragmentCounter, UInt32 time, const string& streamName) {

	AMFWriter& amf = createMessage().writer();
	BinaryWriter& binary(amf.packet);
	binary.write8(type);
	binary.write7BitLongValue(fragmentCounter);
	
	writeAMFStatus(amf, "NetStream.Play.UnpublishNotify", streamName + " is now unpublished");
	flush(false);

	AMFWriter& amf2 = createMessage().writer();
	BinaryWriter& binary2(amf2.packet);
	binary2.write8(type);
	binary2.write7BitLongValue(fragmentCounter);

	writeInvocation(amf2, "closeStream");
	flush();
}*/
