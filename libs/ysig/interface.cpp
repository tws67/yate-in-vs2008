/**
 * interface.cpp
 * This file is part of the YATE Project http://YATE.null.ro 
 *
 * Yet Another Signalling Stack - implements the support for SS7, ISDN and PSTN
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2006 Null Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "yatesig.h"


using namespace TelEngine;

// SignallingInterface notification texts used to print debug
TokenDict SignallingInterface::s_notifName[] = {
	{"LinkUp",     LinkUp},
	{"LinkDown",   LinkDown},
	{"HWError",    HardwareError},
	{"TxClock",    TxClockError},
	{"RxClock",    RxClockError},
	{"Align",      AlignError},
	{"CRC",        CksumError},
	{"TxOversize", TxOversize},
	{"RxOversize", RxOversize},
	{"TxOverflow", TxOverflow},
	{"RxOverflow", RxOverflow},
	{"TxUnder",    TxUnderrun},
	{"RxUnder",    RxUnderrun},
	{0,0}
	};

SignallingInterface::~SignallingInterface()
{
    if (m_receiver)
	Debug(this,DebugGoOn,"Destroyed with receiver (%p) attached",m_receiver);
}

void SignallingInterface::attach(SignallingReceiver* receiver)
{
    Lock lock(m_recvMutex);
    if (m_receiver == receiver)
	return;
    SignallingReceiver* tmp = m_receiver;
    m_receiver = receiver;
    lock.drop();
    if (tmp) {
	const char* name = 0;
	if (engine() && engine()->find(tmp)) {
	    name = tmp->toString().safe();
	    tmp->attach(0);
	}
	Debug(this,DebugAll,"Detached receiver (%p,'%s') [%p]",tmp,name,this);
    }
    if (!receiver)
	return;
    Debug(this,DebugAll,"Attached receiver (%p,'%s') [%p]",
	receiver,receiver->toString().safe(),this);
    insert(receiver);
    receiver->attach(this);
}

bool SignallingInterface::control(Operation oper, NamedList* params)
{
    DDebug(this,DebugInfo,"Unhandled SignallingInterface::control(%d,%p) [%p]",
	oper,params,this);
    return false;
}

bool SignallingInterface::receivedPacket(const DataBlock& packet)
{
    Lock lock(m_recvMutex);
    return m_receiver && m_receiver->receivedPacket(packet);
}

bool SignallingInterface::notify(Notification event)
{
    Lock lock(m_recvMutex);
    return m_receiver && m_receiver->notify(event);
}


SignallingReceiver::~SignallingReceiver()
{
    if (m_interface)
	Debug(this,DebugGoOn,"Destroyed with interface (%p) attached",m_interface);
}

void SignallingReceiver::attach(SignallingInterface* iface)
{
    Lock lock(m_ifaceMutex);
    if (m_interface == iface)
	return;
    SignallingInterface* tmp = m_interface;
    m_interface = iface;
    lock.drop();
    if (tmp) {
	const char* name = 0;
	if (engine() && engine()->find(tmp)) {
	    name = tmp->toString().safe();
	    tmp->attach(0);
	}
	Debug(this,DebugAll,"Detached interface (%p,'%s') [%p]",tmp,name,this);
    }
    if (!iface)
	return;
    Debug(this,DebugAll,"Attached interface (%p,'%s') [%p]",
	iface,iface->toString().safe(),this);
    insert(iface);
    iface->attach(this);
}

bool SignallingReceiver::notify(SignallingInterface::Notification event)
{
    DDebug(this,DebugInfo,"Unhandled SignallingReceiver::notify(%d) [%p]",event,this);
    return false;
}

/* vi: set ts=8 sw=4 sts=4 noet: */
