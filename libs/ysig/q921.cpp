/**
 * q921.cpp
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

/**
 * DEFINEs controlling Q.921 implementation
 * Q921_PASIVE_NOCHECK_PF
 *	Yes: Received UA/DM responses will be validated without checking the P/F bit
 *	No:  Received UA/DM responses without P/F bit set will be dropped
*/
#ifndef Q921_PASIVE_NOCHECK_PF
    #define Q921_PASIVE_NOCHECK_PF
#endif

static const char* s_linkSideNet = "NET";
static const char* s_linkSideCpe = "CPE";

inline const char* linkSide(bool net)
{
    return net ? s_linkSideNet : s_linkSideCpe;
}

// Drop frame reasons
static const char* s_noState = "Not allowed in this state";
static const char* s_noCfg = "Not allowed by configuration";

// Used to set or compare values that may wrap at 127 boundary
class Modulo128
{
public:
    // Increment a value. Set to 0 if greater the 127
    static inline void inc(u_int8_t& value) {
	    if (value < 127)
		value++;
	    else
		value = 0;
	}

    // Check if a given value is in an interval given by it's margins
    // @param value The value to check
    // @param low The lower margin of the interval
    // @param high The higher margin of the interval
    static inline bool between(u_int8_t value, u_int8_t low, u_int8_t high) {
	    if (low == high)
		return value == low;
	    if (low < high)
		return value >= low && value <= high;
	    // low > high: counter wrapped around
	    return value >= low || value <= high;
	}

    // Get the lower margin of an interval given by it's higher margin and length
    // The interval length is assumed non 0
    // @param high The higher margin of the interval
    // @param len The interval length
    static inline u_int8_t getLow(u_int8_t high, u_int8_t len)
	{ return ((high >= len) ? high - len + 1 : 128 - (len - high)); }

};

/**
 * ISDNQ921
 */
// ****************************************************************************
// NOTE:
// *  Private methods are not thread safe. They are called from public
//      and protected methods which are thread safe
// *  Always drop any lock before calling Layer 3 methods to avoid a deadlock:
//      it may try to establish/release/send data from a different thread
// ****************************************************************************

// Constructor. Set data members. Print them
ISDNQ921::ISDNQ921(const NamedList& params, const char* name)
    : ISDNLayer2(params,name),
      SignallingReceiver(),
      m_remoteBusy(false),
      m_timerRecovery(false),
      m_rejectSent(false),
      m_pendingDMSabme(false),
      m_lastPFBit(false),
      m_vs(0),
      m_va(0),
      m_vr(0),
      m_layer(true),
      m_retransTimer(0),
      m_idleTimer(0),
      m_window(7),
      m_n200(3),
      m_txFrames(0),
      m_txFailFrames(0),
      m_rxFrames(0),
      m_rxRejectedFrames(0),
      m_rxDroppedFrames(0),
      m_hwErrors(0),
      m_dumper(0),
      m_printFrames(true),
      m_extendedDebug(false),
      m_errorSend(false),
      m_errorReceive(false)
{
    setName(params.getValue("debugname",name));
    m_retransTimer.interval(params,"t200",1000,1000,false);
    m_idleTimer.interval(params,"t203",2000,10000,false);
    // Adjust idle timeout to data link side
    m_idleTimer.interval(m_idleTimer.interval() + (network() ? -500 : 500));
    m_window.maxVal(params.getIntValue("maxpendingframes",7));
    if (!m_window.maxVal())
	m_window.maxVal(7);
    setDebug(params.getBoolValue("print-frames",false),
	params.getBoolValue("extended-debug",false));
    if (debugAt(DebugInfo)) {
	String tmp;
#ifdef DEBUG
	tmp << " SAPI/TEI=" << (unsigned int)sapi() << "/" << (unsigned int)tei();
	tmp << " auto-restart=" << String::boolText(autoRestart());
	tmp << " max-user-data=" << (unsigned int)maxUserData();
	tmp << " max-pending-frames: " << (unsigned int)m_window.maxVal();
	tmp << " retrans/idle=" << (unsigned int)m_retransTimer.interval()  << "/"
		<< (unsigned int)m_idleTimer.interval();
	tmp << " allow-unack-data=" << String::boolText(allowUnack());
#endif
	Debug(this,DebugInfo,"ISDN Data Link type=%s%s [%p]",
	    linkSide(network()),tmp.safe(),this);
    }
}

// Destructor
ISDNQ921::~ISDNQ921()
{
    Lock lock(m_layer);
    ISDNLayer2::attach(0);
    SignallingReceiver::attach(0);
    cleanup();
    if (debugAt(DebugAll))
	Debug(this,DebugAll,
	    "ISDN Data Link destroyed. Frames: sent=%u (failed=%u) recv=%u rejected=%u dropped=%u. HW errors=%u [%p]",
	    (unsigned int)m_txFrames,(unsigned int)m_txFailFrames,
	    (unsigned int)m_rxFrames,(unsigned int)m_rxRejectedFrames,
	    (unsigned int)m_rxDroppedFrames,(unsigned int)m_hwErrors,this);
}

// Set or release 'multiple frame acknoledged' mode
bool ISDNQ921::multipleFrame(bool establish, bool force)
{
    Lock lock(m_layer);
    // Check state. Don't do nothing in transition states
    if (state() == WaitEstablish || state() == WaitRelease)
	return false;
    // The request wouldn't change our state and we are not forced to fulfill it
    if (!force &&
	((establish && (state() == Established || state() == WaitEstablish)) ||
	(!establish && (state() == Released || state() == WaitRelease))))
	return false;
    XDebug(this,DebugAll,"Process '%s' request",establish ? "ESTABLISH" : "RELEASE");
    bool result;
    if (establish) {
	reset();
	result = sendUFrame(ISDNFrame::SABME,true,true);
	changeState(WaitEstablish);
	timer(true,false);
    }
    else {
	// Already disconnected: Just notify Layer 3
	if (state() == Released) {
	    lock.drop();
	    multipleFrameReleased(true,false);
	    return true;
	}
	reset();
	result = sendUFrame(ISDNFrame::DISC,true,true);
	changeState(WaitRelease);
	timer(true,false);
    }
    return result;
}

// Send data through the HDLC interface
bool ISDNQ921::sendData(const DataBlock& data, bool ack)
{
    Lock lock(m_layer);
    if (!(data.length() && teiAssigned()))
	return false;
    if (ack) {
	if (state() == Released || m_window.full())
	    return false;
	// Enqueue and send outgoing data
	ISDNFrame* f = new ISDNFrame(true,network(),sapi(),tei(),false,data);
	// Update frame send seq number. Inc our send seq number and window counter
	f->update(&m_vs,0);
	Modulo128::inc(m_vs);
	m_window.inc();
	// Append and try to send frame
	m_outFrames.append(f);
	DDebug(this,DebugAll,
	    "Enqueued data frame (%p). Sequence number: %u",f,f->ns());
	sendOutgoingData();
	return true;
    }
    // Unacknoledged data request
    if (!allowUnack())
	return false;
    // P/F bit is always false for UI frames. See Q.921 5.2.2
    ISDNFrame* f = new ISDNFrame(false,network(),sapi(),tei(),false,data);
    bool result = sendFrame(f);
    f->deref();
    return result;
}

// Send DISC. Reset data
void ISDNQ921::cleanup()
{
    Lock lock(m_layer);
    DDebug(this,DebugAll,"Cleanup in state '%s'",stateName(state()));
    // Don't send DISC if we are disconnected or waiting to become disconnected
    if (state() == Established)
	sendUFrame(ISDNFrame::DISC,true,true);
    reset();
    changeState(Released);
}

void* ISDNQ921::getObject(const String& name) const
{
    if (name == "ISDNQ921")
	return (void*)this;
    return 0;
}

// Method called periodically to check timeouts
// Re-sync with remote peer if necessary
void ISDNQ921::timerTick(const Time& when)
{
    Lock lock(m_layer);
    if (state() == Released)
	return;
    // T200 not started
    if (!m_retransTimer.started()) {
	// T203 not started: START
	if (!m_idleTimer.started()) {
	    timer(false,true,when.msec());
	    m_timerRecovery = false;
	    return;
	}
	// T203 started: Timeout ?
	if (!m_idleTimer.timeout(when.msec()))
	    return;
	// Start timer
	XDebug(this,DebugInfo,"T203 expired. Start T200");
	timer(true,false,when.msec());
    }
    // T200 started
    if (!m_retransTimer.timeout(when.msec()))
	return;
    // Q.921 5.6.7: Timeout
    // Done all retransmissions ?
    if (m_n200.full()) {
	DDebug(this,DebugNote,"Timeout. Link is down");
	reset();
	changeState(Released);
	lock.drop();
	multipleFrameReleased(false,true);
	if (autoRestart())
	    multipleFrame(true,false);
	return;
    }
    // Waiting to establish/release ?
    if (state() == WaitEstablish || state() == WaitRelease) {
	ISDNFrame::Type t = (state() == WaitEstablish) ?
	    ISDNFrame::SABME : ISDNFrame::DISC;
	XDebug(this,DebugAll,"T200 expired. Retransmit '%s'",ISDNFrame::typeName(t));
	sendUFrame(t,true,true,true);
	m_n200.inc();
	timer(true,false,when.msec());
	return;
    }
    // State is Established
    if (!m_timerRecovery) {
	m_n200.reset();
	m_timerRecovery = true;
    }
    // Try to retransmit some data or send RR
    if (!sendOutgoingData(true)) {
	XDebug(this,DebugAll,"T200 expired. Send '%s'",ISDNFrame::typeName(ISDNFrame::RR));
	sendSFrame(ISDNFrame::RR,true,true);
	m_lastPFBit = true;
    }
    m_n200.inc();
    timer(true,false,when.msec());
}

// Process a packet received by the receiver's interface
// Parse data. Validate received frame and process it
bool ISDNQ921::receivedPacket(const DataBlock& packet)
{
    if (!packet.length())
	return false;
    Lock lock(m_layer);
    XDebug(this,DebugAll,"Received packet (Length: %u)",packet.length());
    ISDNFrame* frame = ISDNFrame::parse(packet,this);
    if (!frame) {
	if (!m_errorReceive)
	    Debug(this,DebugNote,"Received short data (Length: %u)",packet.length());
	m_errorReceive = true;
	return false;
    }
    m_errorReceive = false;
    // Print & dump
    if (debugAt(DebugInfo) && m_printFrames) {
	String tmp;
	frame->toString(tmp,m_extendedDebug);
	Debug(this,DebugInfo,"Received frame (%p):%s",frame,tmp.c_str());
    }
    if (m_dumper && frame->type() < ISDNFrame::Invalid)
	m_dumper->dump(frame->buffer(),false);
    // Accept
    bool reject = false;
    // Not accepted:
    // If not rejected, for out of range sequence number send
    //     REJ to request retransmission if not already sent or RR to confirm if REJ already sent
    //     Just drop the frame otherwise
    // If rejected (unrecoverable error), re-establish data link
    if (!acceptFrame(frame,reject)) {
	if (!reject) {
	    if (frame->m_error == ISDNFrame::ErrTxSeqNo) {
		if (!m_rejectSent) {
		    sendSFrame(ISDNFrame::REJ,true,true);
		    m_rejectSent = true;
		    m_lastPFBit = true;
		}
		else
		    sendSFrame(ISDNFrame::RR,false,frame->poll());
	    }
	    frame->deref();
	    return true;
	}
	// Unrecoverable error: re-establish
	Debug(this,DebugNote,"Rejected frame (%p): %s. Reason: '%s'. Restarting",
	    frame,frame->name(),ISDNFrame::typeName(frame->error()));
	frame->deref();
	reset();
	changeState(WaitEstablish);
	sendUFrame(ISDNFrame::SABME,true,true);
	timer(true,false);
	return true;
    }
    // Process
    XDebug(this,DebugAll,"Process frame (%p): '%s' in state '%s'",
	frame,frame->name(),ISDNLayer2::stateName(state()));
    bool chgState = false, confirmation = false;
    State newState;
    if (frame->category() == ISDNFrame::Data) {
	bool ack = (frame->type() == ISDNFrame::I);
	if (processDataFrame(frame,ack)) {
	    DataBlock tmp;
	    frame->getData(tmp);
	    lock.drop();
	    receiveData(tmp,ack);
	}
	frame->deref();
	return true;
    }
    if (frame->category() == ISDNFrame::Supervisory) {
	if (processSFrame(frame)) {
	    // Exit from timer recovery
	    m_timerRecovery = false;
	    if (m_pendingDMSabme) {
		m_pendingDMSabme = false;
		chgState = true;
		newState = WaitEstablish;
	    }
	}
    }
    else
	chgState = processUFrame(frame,newState,confirmation);
    frame->deref();
    // Change state ?
    if (!chgState)
	return true;
    reset();
    changeState(newState);
    switch (newState) {
	case Established:
	    timer(false,true);
	    lock.drop();
	    multipleFrameEstablished(confirmation,false);
	    break;
	case Released:
	    lock.drop();
	    multipleFrameReleased(confirmation,false);
	    break;
	case WaitEstablish:
	    sendUFrame(ISDNFrame::SABME,true,true);
	    timer(true,false);
	    break;
	case WaitRelease:
	    sendUFrame(ISDNFrame::DISC,true,true);
	    timer(true,false);
	    break;
    }
    return true;
}

// Process a notification generated by the attached interface
bool ISDNQ921::notify(SignallingInterface::Notification event)
{
    Lock lock(m_layer);
    if (event != SignallingInterface::LinkUp)
	m_hwErrors++;
    else {
	Debug(this,DebugInfo,"Received notification %u: '%s'",
	    event,lookup(event,SignallingInterface::s_notifName));
	return true;
    }
    if (event == SignallingInterface::LinkDown) {
	Debug(this,DebugWarn,"Received notification %u: '%s'",
	    event,lookup(event,SignallingInterface::s_notifName));
	reset();
	changeState(Released);
	lock.drop();
	multipleFrameReleased(false,false);
	if (autoRestart())
	    multipleFrame(true,false);
	return true;
    }
#ifdef DEBUG
    if (!(m_hwErrors % 250))
	DDebug(this,DebugNote,"Received notification %u: '%s'. Total=%u",
	    event,lookup(event,SignallingInterface::s_notifName,"Undefined"),m_hwErrors);
#endif
    return true;
}

// Reset data
void ISDNQ921::reset()
{
    Lock lock(m_layer);
    XDebug(this,DebugAll,"Reset");
    m_remoteBusy = false;
    m_timerRecovery = false;
    m_rejectSent = false;
    m_lastPFBit = false;
    m_n200.reset();
    m_window.reset();
    timer(false,false);
    m_outFrames.clear();
    m_va = m_vs = m_vr = 0;
}

// Set/remove data dumper
void ISDNQ921::setDumper(SignallingDumper* dumper)
{
    Lock lock(m_layer);
    if (m_dumper == dumper)
	return;
    SignallingDumper* tmp = m_dumper;
    m_dumper = dumper;
    delete tmp;
    XDebug(this,DebugAll,"Data dumper set to (%p)",m_dumper);
}

// Acknoledge pending outgoing frames. See Q.921 5.6.3.2
// Remove ack'd frames from queue. Start idle timer
bool ISDNQ921::ackOutgoingFrames(const ISDNFrame* frame)
{
    bool ack = false, unack = false;
    // Acknoledge frames with N(S) up to frame->nr() (not including)
    for (;;) {
	ObjList* obj = m_outFrames.skipNull();
	ISDNFrame* f = obj ? static_cast<ISDNFrame*>(obj->get()) : 0;
	// Stop when no frames or seq number equals nr
	if (!f || frame->nr() == f->ns()) {
	    if (f && f->sent())
		unack = true;
	    break;
	}
	ack = true;
	DDebug(this,DebugAll,
	    "Remove acknoledged data frame (%p). Sequence number: %u",f,f->ns());
	m_window.dec();
	m_outFrames.remove(f,true);
    }
    // Reset T200 if not in timer-recovery condition and ack some frame
    // 5.5.3.2: Note 1: Dont't reset if we've requested a response and haven't got one
    if (!m_timerRecovery && ack &&
	!(frame->type() != ISDNFrame::I && m_lastPFBit))
	timer(false,false);
    // Start T200 if we have unacknoledged data and not already started
    if (unack && !m_retransTimer.started())
	timer(true,false);
    return ack;
}

// Receive I/UI (data) frames (See Q.921 5.6.2)
// Send unacknoledged data to upper layer
// Ack pending outgoing data and confirm (by sending any pending data or an RR confirmation)
bool ISDNQ921::processDataFrame(const ISDNFrame* frame, bool ack)
{
    const char* reason = 0;
    // State or configuration allow receiving data ?
    if (ack) {
	if (state() != Established)
	    reason = s_noState;
    }
    else {
	if (!allowUnack())
	    reason = s_noCfg;
    }
    if (reason) {
	dropFrame(frame,reason);
	return false;
    }
    // Done for unacknoledged (UI frame) data
    if (!ack)
	return true;
    // Acknoledged data
    m_rejectSent = false;
    m_remoteBusy = false;
    m_vr = frame->ns();
    Modulo128::inc(m_vr);
    XDebug(this,DebugAll,"Set V(R) to %u",m_vr);
    ackOutgoingFrames(frame);
    m_va = frame->nr();
    XDebug(this,DebugAll,"Set V(A) to %u.",m_va);
    // P/F=1: Q.921 5.6.2.1   P/F=0: Q.921 5.6.2.2
    if (frame->poll())
	sendSFrame(ISDNFrame::RR,false,true);
    else
	if (!sendOutgoingData())
	    sendSFrame(ISDNFrame::RR,false,false);
    // Start T203 if T200 not started
    if (!m_retransTimer.started())
	timer(false,true);
    return true;
}

// Process received S (supervisory) frames: RR, REJ, RNR
// All   Ack outgoing frames. Respond with RR if requested
// RR    Send pending frames. Start idle timer
// REJ   Send pending frames. Adjust send frame and expected frame counter if necessary
// RNR   Adjust send frame counter if necessary
bool ISDNQ921::processSFrame(const ISDNFrame* frame)
{
    if (state() != Established) {
	dropFrame(frame,s_noState);
	return false;
    }
    if (frame->type() == ISDNFrame::RR) {
	// Ack sent data. Send unsent data
	// Respond if it's an unsolicited frame with P/F set to 1
	m_remoteBusy = false;
	ackOutgoingFrames(frame);
	bool sent = sendOutgoingData();
	if (frame->poll()) {
	    // Check if we requested a response. If not, respond if it is a command
	    if (!m_lastPFBit && frame->command())
		sendSFrame(ISDNFrame::RR,false,true);
	    // Don't reset if we've sent any data
	    if (!sent) {
		m_lastPFBit = false;
		timer(false,true);
	    }
	}
	if (!m_retransTimer.started() && !m_idleTimer.started())
	    timer(false,true);
	return false;
    }
    // Q.921 5.6.4: Receiving REJ frames
    if (frame->type() == ISDNFrame::REJ) {
	m_remoteBusy = false;
	// Ack sent data.
	ackOutgoingFrames(frame);
	// Q.921 5.6.4 a) and b)
	bool rspPF = !frame->command() && frame->poll();
	if (!m_timerRecovery || (m_timerRecovery && rspPF)) {
	    m_vs = m_va = frame->nr();
	    XDebug(this,DebugAll,"Set V(S) and V(A) to %u.",m_vs);
	    if (!m_timerRecovery && frame->command() && frame->poll())
		sendSFrame(ISDNFrame::RR,false,true);
	    // Retransmit only if we didn't sent a supervisory frame
	    if (!m_lastPFBit) {
		bool t200 = sendOutgoingData(true);
		timer(t200,!t200);
	    }
	    if (!m_timerRecovery && rspPF)
		Debug(this,DebugNote,"Frame (%p) is a REJ response with P/F set",frame);
	    m_timerRecovery = false;
	    return false;
	}
	// Q.921 5.6.4 c)
	m_va = frame->nr();
	XDebug(this,DebugAll,"Set V(A) to %u.",m_va);
	if (frame->command() && frame->poll())
	    sendSFrame(ISDNFrame::RR,false,true);
	return false;
    }
    // Q.921 5.6.5: Receiving RNR frames
    if (frame->type() == ISDNFrame::RNR) {
	m_remoteBusy = true;
	// Ack sent data.
	ackOutgoingFrames(frame);
	// Respond
	if (frame->poll())
	    if (frame->command())
		sendSFrame(ISDNFrame::RR,false,true);
	    else {
		m_timerRecovery = false;
		m_vs = frame->nr();
		XDebug(this,DebugAll,"Set V(S) to %u.",m_vs);
	    }
	if (!m_lastPFBit)
	    timer(true,false);
	return false;
    }
    dropFrame(frame,s_noState);
    return false;
}

//  Receive U frames: UA, DM, SABME, DISC, FRMR
//  UA    If P/F = 0: DROP - not a valid response
//        State is Wait...: it's a valid response: notify layer 3 and change state
//        Otherwise: DROP
//  DM    State is Established or Released
//            P/F = 0: It's an establish request. Send SABME. Change state
//            P/F = 1: If state is Established and timer recovery: schedule establish
//        State is WaitEstablish or WaitRelease and P/F = 1: Release. Notify layer 3
//        Otherwise: DROP
//  SABME State is Established or Released: Confirm. Notify layer 3. Reset
//        State is WaitEstablish: Just confirm
//        State is WaitRelease: Send DM. Release. Notify layer 3
//  DISC  State is Established: Confirm. Release. Notify layer 3
//        State is Released: Just send a DM response
//        State is WaitEstablish: Send DM response. Release. Notify layer 3
//        State is WaitRelease: Just confirm
//  FRMR  If state is Established: re-establish
//        Otherwise: DROP
bool ISDNQ921::processUFrame(const ISDNFrame* frame, State& newState,
	bool& confirmation)
{
    switch (frame->type()) {
	case ISDNFrame::UA:
	    if (!(frame->poll() &&
		(state() == WaitEstablish || state() == WaitRelease)))
		break;
	    newState = (state() == WaitEstablish ? Established : Released);
	    confirmation = true;
	    return true;
	case ISDNFrame::DM:
	    if (state() == Established || state() == Released) {
		if (!frame->poll()) {
		    newState = WaitEstablish;
		    return true;
		}
		if (state() == Established && m_timerRecovery) {
		    m_pendingDMSabme = true;
		    return false;
		}
	    }
	    if (frame->poll()) {
		newState = Released;
		confirmation = true;
		return true;
	    }
	    break;
	case ISDNFrame::SABME:
	    if (state() == Established || state() == Released) {
		sendUFrame(ISDNFrame::UA,false,frame->poll());
		newState = Established;
		confirmation = false;
		return true;
	    }
	    if (state() == WaitEstablish) {
		sendUFrame(ISDNFrame::UA,false,frame->poll());
		return false;
	    }
	    sendUFrame(ISDNFrame::DM,false,frame->poll());
	    newState = Released;
	    confirmation = true;
	    return true;
	case ISDNFrame::DISC:
	    switch (state()) {
		case Established:
		    sendUFrame(ISDNFrame::UA,false,frame->poll());
		    newState = Released;
		    confirmation = false;
		    return true;
		case Released:
		    sendUFrame(ISDNFrame::DM,false,frame->poll());
		    return false;
		case WaitEstablish:
		    sendUFrame(ISDNFrame::DM,false,frame->poll());
		    newState = Released;
		    confirmation = true;
		    return true;
		case WaitRelease:
		    sendUFrame(ISDNFrame::UA,false,frame->poll());
		    return false;
	    }
	    break;
	case ISDNFrame::FRMR:
	    if (state() == Established) {
		newState = WaitEstablish;
		return true;
	    }
	    break;
	default: ;
    }
    dropFrame(frame,s_noState);
    return false;
}

// Accept frame according to Q.921 5.8.5. Reasons to reject:
//	Unknown command/response
//	Invalid N(R)
//	Information field too long
// Update receive counters
bool ISDNQ921::acceptFrame(ISDNFrame* frame, bool& reject)
{
    reject = false;
    // Update received frames
    m_rxFrames++;
    // Check frame only if it's not already invalid
    for (; frame->error() < ISDNFrame::Invalid;) {
	// Check SAPI/TEI
	if (frame->sapi() != sapi() || frame->tei() != tei()) {
	    frame->m_error = ISDNFrame::ErrInvalidAddress;
	    break;
	}
	// Drop out of range I frames
	if (frame->type() == ISDNFrame::I && frame->ns() != m_vr) {
	    frame->m_error = ISDNFrame::ErrTxSeqNo;
	    break;
	}
	// Check DISC/SABME commands and UA/DM responses
	if (((frame->type() == ISDNFrame::SABME || frame->type() == ISDNFrame::DISC) &&
	    !frame->command()) ||
	    ((frame->type() == ISDNFrame::UA || frame->type() == ISDNFrame::DM) &&
	    frame->command())) {
	    Debug(this,DebugGoOn,
		"Received '%s': The remote peer has the same data link side type",
		frame->name());
	    frame->m_error = ISDNFrame::ErrInvalidCR;
	    break;
	}
	// We don't support XID
	if (frame->type() == ISDNFrame::XID) {
	    frame->m_error = ISDNFrame::ErrUnsupported;
	    break;
	}
	// Check N(R) for I or S frames (N(R) is set to 0xFF for U frames):
	// N(R) should be between V(A) and V(S)
	if (frame->nr() < 128 && !Modulo128::between(frame->nr(),m_va,m_vs)) {
	    frame->m_error = ISDNFrame::ErrRxSeqNo;
	    break;
	}
	// Check data length
	if (frame->dataLength() > maxUserData()) {
	    frame->m_error = ISDNFrame::ErrDataLength;
	    break;
	}
	break;
    }
    // Accepted
    if (frame->error() < ISDNFrame::Invalid)
	return true;
    // Frame is invalid. Reject or drop ?
    if (frame->error() == ISDNFrame::ErrUnknownCR ||
	frame->error() == ISDNFrame::ErrRxSeqNo ||
	frame->error() == ISDNFrame::ErrDataLength) {
	// Check if the state allows the rejection. Not allowed if:
	//  - Not in multiple frame operation mode
	if (state() == Established) {
	    m_rxRejectedFrames++;
	    reject = true;
	    return false;
	}
    }
    dropFrame(frame,ISDNFrame::typeName(frame->error()));
    return false;
}

void ISDNQ921::dropFrame(const ISDNFrame* frame, const char* reason)
{
    m_rxDroppedFrames++;
    DDebug(this,DebugNote,
	"Dropping frame (%p): %s. Reason: %s. V(S),V(R),V(A)=%u,%u,%u",
	frame,frame->name(),reason,m_vs,m_vr,m_va);
}

// Send U frames except for UI frames
bool ISDNQ921::sendUFrame(ISDNFrame::Type type, bool command, bool pf,
	bool retrans)
{
    switch (type) {
	case ISDNFrame::SABME:
	case ISDNFrame::DISC:
	case ISDNFrame::DM:
	case ISDNFrame::UA:
	case ISDNFrame::FRMR:
	    break;
	default:
	    return false;
    }
    // Create and send frame
    // U frames don't have an N(R) control data
    ISDNFrame* f = new ISDNFrame(type,command,network(),sapi(),tei(),pf);
    f->sent(retrans);
    bool result = sendFrame(f);
    f->deref();
    return result;
}

// Send S frames
bool ISDNQ921::sendSFrame(ISDNFrame::Type type, bool command, bool pf)
{
    if (!(type == ISDNFrame::RR ||
	type == ISDNFrame::RNR ||
	type == ISDNFrame::REJ))
	return false;
    // Create and send frame
    ISDNFrame* f = new ISDNFrame(type,command,network(),sapi(),tei(),pf,m_vr);
    bool result = sendFrame(f);
    f->deref();
    return result;
}

// Send a frame to remote peer. Dump data on success if we have a dumper
bool ISDNQ921::sendFrame(const ISDNFrame* frame)
{
    if (!frame)
	return false;
    // This should never happen !!!
    if (frame->type() >= ISDNFrame::Invalid) {
	Debug(this,DebugWarn,"Refusing to send '%s' frame",frame->name());
	return false;
    }
    // Print frame
    if (debugAt(DebugInfo) && m_printFrames && !m_errorSend) {
	String tmp;
	frame->toString(tmp,m_extendedDebug);
	Debug(this,DebugInfo,"Sending frame (%p):%s",
	    frame,tmp.c_str());
    }
    bool result = SignallingReceiver::transmitPacket(frame->buffer(),
	false,SignallingInterface::Q921);
    // Dump frame if no error and we have a dumper
    if (result) {
	m_txFrames++;
	if (m_dumper)
	    m_dumper->dump(frame->buffer(),true);
	m_errorSend = false;
    }
    else {
	m_txFailFrames++;
	if (!m_errorSend)
	    Debug(this,DebugNote,"Error sending frame (%p): %s",frame,frame->name());
	m_errorSend = true;
    }
    return result;
}

// Send (or re-send) enqueued data frames
bool ISDNQ921::sendOutgoingData(bool retrans)
{
    bool sent = false;
    for (;;) {
	if (m_remoteBusy || m_window.empty())
	    break;
	ObjList* obj = m_outFrames.skipNull();
	// Queue empty ?
	if (!obj)
	    break;
	ISDNFrame* frame = 0;
	// Not a retransmission: skip already sent frames
	if (!retrans)
	    for (; obj; obj = obj->skipNext()) {
		frame = static_cast<ISDNFrame*>(obj->get());
		if (!frame->sent())
		    break;
	    }
	// Send the remaining unsent frames in window or
	//  the whole queue if it is a retransmission
	for (; obj ; obj = obj->skipNext()) {
	    frame = static_cast<ISDNFrame*>(obj->get());
	    // Update frame receive sequence number
	    frame->update(0,&m_vr);
	    XDebug(this,DebugAll,
		"Sending data frame (%p). Sequence number: %u. Retransmission: %s",
		frame,frame->ns(),String::boolText(frame->sent()));
	    // T200
	    if (!m_retransTimer.started())
		timer(true,false);
	    // Send
	    sendFrame(frame);
	    sent = true;
	    frame->sent(true);
	}
	break;
    }
    return sent;
}

// Start/stop idle or retransmission timers
void ISDNQ921::timer(bool start, bool t203, u_int64_t time)
{
    if (start) {
	if (m_idleTimer.started()) {
	    m_idleTimer.stop();
	    XDebug(this,DebugAll,"T203 stopped");
	}
	// Start anyway. Even if already started
	if (!time)
	     time = Time::msecNow();
	m_retransTimer.start(time);
	XDebug(this,DebugAll,"T200 started. Transmission counter: %u",
	    m_n200.count());
    }
    else {
	m_n200.reset();
	if (m_retransTimer.started()) {
	    m_retransTimer.stop();
	    XDebug(this,DebugAll,"T200 stopped");
	}
	if (t203)
	    if (!m_idleTimer.started()) {
		if (!time)
		     time = Time::msecNow();
		m_idleTimer.start(time);
		XDebug(this,DebugAll,"T203 started");
	    }
	else
	    if (m_idleTimer.started()) {
		m_idleTimer.stop();
		XDebug(this,DebugAll,"T203 stopped");
	    }
    }
}

/**
 * ISDNQ921Pasive
 */
// Constructor. Set data members. Print them
ISDNQ921Pasive::ISDNQ921Pasive(const NamedList& params, const char* name)
    : ISDNLayer2(params,name),
      SignallingReceiver(),
      m_layer(true),
      m_checkLinkSide(false),
      m_idleTimer(0),
      m_lastFrame(255),
      m_rxFrames(0),
      m_rxRejectedFrames(0),
      m_rxDroppedFrames(0),
      m_hwErrors(0),
      m_dumper(0),
      m_printFrames(true),
      m_extendedDebug(false),
      m_errorReceive(false)
{
    setName(params.getValue("debugname",name));
    m_idleTimer.interval(params,"idletimeout",4000,30000,false);
    m_checkLinkSide = detectType();
    setDebug(params.getBoolValue("print-frames",false),
	params.getBoolValue("extended-debug",false));
    Debug(this,DebugInfo,
	"ISDN Passive Data Link type=%s autodetect=%s idle-timeout=%u [%p]",
	linkSide(network()),String::boolText(detectType()),
	(unsigned int)m_idleTimer.interval(),this);
    m_idleTimer.start();
}

// Destructor
ISDNQ921Pasive::~ISDNQ921Pasive()
{
    Lock lock(m_layer);
    ISDNLayer2::attach(0);
    SignallingReceiver::attach(0);
    cleanup();
    if (debugAt(DebugAll))
	Debug(this,DebugAll,
	    "ISDN Passive Data Link destroyed. Frames: recv=%u rejected=%u dropped=%u. HW errors=%u [%p]",
	    (unsigned int)m_rxFrames,(unsigned int)m_rxRejectedFrames,
	    (unsigned int)m_rxDroppedFrames,(unsigned int)m_hwErrors,this);
}

// Reset data
void ISDNQ921Pasive::cleanup()
{
    Lock lock(m_layer);
    m_idleTimer.start();
}

// Get data members pointers
void* ISDNQ921Pasive::getObject(const String& name) const
{
    if (name == "ISDNQ921Pasive")
	return (void*)this;
    return 0;
}

// Set/remove data dumper
void ISDNQ921Pasive::setDumper(SignallingDumper* dumper)
{
    Lock lock(m_layer);
    if (m_dumper == dumper)
	return;
    SignallingDumper* tmp = m_dumper;
    m_dumper = dumper;
    delete tmp;
    XDebug(this,DebugAll,"Data dumper set to (%p)",m_dumper);
}

// Called periodically by the engine to check timeouts
// Check idle timer. Notify upper layer on timeout
void ISDNQ921Pasive::timerTick(const Time& when)
{
    Lock lock(m_layer);
    if (!m_idleTimer.timeout(when.msec()))
	return;
    // Timeout. Notify layer 3. Restart timer
    XDebug(this,DebugNote,"Timeout. Channel was idle for " FMT64 " ms",m_idleTimer.interval());
    m_idleTimer.start(when.msec());
    lock.drop();
    idleTimeout();
}

// Process a packet received by the receiver's interface
bool ISDNQ921Pasive::receivedPacket(const DataBlock& packet)
{
    if (!packet.length())
	return false;
    Lock lock(m_layer);
    XDebug(this,DebugAll,"Received packet (Length: %u)",packet.length());
    ISDNFrame* frame = ISDNFrame::parse(packet,this);
    if (!frame) {
	if (!m_errorReceive)
	    Debug(this,DebugNote,"Received short data (Length: %u)",packet.length());
	m_errorReceive = true;
	return false;
    }
    m_errorReceive = false;
    // Print & dump
    if (debugAt(DebugInfo) && m_printFrames) {
	String tmp;
	frame->toString(tmp,m_extendedDebug);
	Debug(this,DebugInfo,"Received frame (%p):%s",frame,tmp.c_str());
    }
    if (m_dumper && frame->type() < ISDNFrame::Invalid)
	m_dumper->dump(frame->buffer(),false);
    // Received enough data to parse. Assume the channel not idle (restart timer)
    // If accepted, the frame is a data frame or a unnumbered (SABME,DISC,UA,DM) one
    //   Drop retransmissions of data frames
    //   Send data or notification to the upper layer
    m_idleTimer.start();
    lock.drop();
    bool cmd,value;
    if (acceptFrame(frame,cmd,value)) {
	if (frame->category() == ISDNFrame::Data) {
	    if (m_lastFrame != frame->ns()) {
		DataBlock tmp;
		frame->getData(tmp);
		m_lastFrame = frame->ns();
		receiveData(tmp,frame->type() == ISDNFrame::I);
	    }
	}
	else
	    dataLinkState(cmd,value);
    }
    frame->deref();
    return true;
}

// Process a notification generated by the attached interface
bool ISDNQ921Pasive::notify(SignallingInterface::Notification event)
{
    Lock lock(m_layer);
    if (event != SignallingInterface::LinkUp)
	m_hwErrors++;
    else {
	Debug(this,DebugInfo,"Received notification %u: '%s'",
	    event,lookup(event,SignallingInterface::s_notifName));
	return true;
    }
    if (event == SignallingInterface::LinkDown)
	Debug(this,DebugWarn,"Received notification %u: '%s'",
	    event,lookup(event,SignallingInterface::s_notifName));
#ifdef DEBUG
    else if (!(m_hwErrors % 250))
	Debug(this,DebugNote,"Received notification %u: '%s'. Total=%u",
	    event,lookup(event,SignallingInterface::s_notifName,"Undefined"),m_hwErrors);
#endif
    return true;
}

// Accept frame according to Q.921 5.8.5
// Filter received frames. Accept only frames that would generate a notification to the upper layer:
// UI/I and valid SABME/DISC/UA/DM
bool ISDNQ921Pasive::acceptFrame(ISDNFrame* frame, bool& cmd, bool& value)
{
    // Update received frames
    m_rxFrames++;
    // Frame already invalid
    if (frame->error() >= ISDNFrame::Invalid)
	return dropFrame(frame);
    // Check SAPI/TEI
    if (frame->sapi() != sapi() || frame->tei() != tei())
	return dropFrame(frame,ISDNFrame::typeName(ISDNFrame::ErrInvalidAddress));
    // Valid UI/I
    if (frame->category() == ISDNFrame::Data)
	return true;
    // Check DISC/SABME commands and UA/DM responses
    cmd = (frame->type() == ISDNFrame::SABME || frame->type() == ISDNFrame::DISC);
    bool response = (frame->type() == ISDNFrame::UA || frame->type() == ISDNFrame::DM);
    if (m_checkLinkSide &&
	((cmd && !frame->command()) || (response && frame->command()))) {
	if (detectType()) {
	    m_checkLinkSide = false;
	    changeType();
	}
	else {
	    Debug(this,DebugGoOn,
		"Received '%s': The remote peer has the same data link side type",
		frame->name());
	    return dropFrame(frame,ISDNFrame::typeName(ISDNFrame::ErrInvalidCR));
	}
    }
    // Normally, SABME/DISC commands and UA/DM responses should have the P/F bit set
    if (cmd || response) {
	if (!frame->poll())
#ifndef Q921_PASIVE_NOCHECK_PF
	    return dropFrame(frame,"P/F bit not set");
#else
	    DDebug(this,DebugNote,"Received '%s' without P/F bit set",frame->name());
#endif
	m_checkLinkSide = detectType();
	if (cmd)
	    value = (frame->type() == ISDNFrame::SABME);
	else
	    value = (frame->type() == ISDNFrame::UA);
	return true;
    }
    // Drop valid frames without debug message (it would be too much) and without counting them:
    //    Supervisory frames (Since we don't synchronize, we don't process them)
    //    Unsupported valid unnumbered frames (e.g. XID, UA/DM with P/F bit set ....)
    if (frame->type() < ISDNFrame::Invalid)
	return false;
    return dropFrame(frame);
}

bool ISDNQ921Pasive::dropFrame(const ISDNFrame* frame, const char* reason)
{
    m_rxDroppedFrames++;
    DDebug(this,DebugNote,"Dropping frame (%p): %s. Reason: %s",
	frame,frame->name(),reason ? reason : ISDNFrame::typeName(frame->error()));
    return false;
}

/**
 * ISDNLayer2
 */
TokenDict ISDNLayer2::m_states[] = {
	{"Released",      Released},
	{"WaitEstablish", WaitEstablish},
	{"Established",   Established},
	{"WaitRelease",   WaitRelease},
	{0,0}
	};

ISDNLayer2::ISDNLayer2(const NamedList& params, const char* name)
    : SignallingComponent(name),
      m_layer3(0),
      m_interfaceMutex(true),
      m_layer3Mutex(true),
      m_state(Released),
      m_network(false),
      m_detectType(false),
      m_sapi(0),
      m_tei(0),
      m_teiAssigned(false),
      m_allowUnack(false),
      m_autoRestart(true),
      m_maxUserData(260)
{
    setName(params.getValue("debugname",name));
    XDebug(this,DebugAll,"ISDNLayer2");
    m_network = params.getBoolValue("network",false);
    m_detectType = params.getBoolValue("detect",false);
    int tmp = params.getIntValue("sapi",0);
    m_sapi = (tmp  >= 0 && tmp <= 63) ? tmp : 0;
    tmp = params.getIntValue("tei",0);
    m_tei = (tmp  >= 0 && tmp <= 127) ? tmp : 0;
    teiAssigned(true);
    m_allowUnack = params.getBoolValue("allow-unack",false);
    m_autoRestart = params.getBoolValue("auto-restart",true);
    m_maxUserData = params.getIntValue("maxuserdata",260);
    if (!m_maxUserData)
	m_maxUserData = 260;
}

ISDNLayer2::~ISDNLayer2()
{
    if (m_layer3)
	Debug(this,DebugGoOn,"Destroyed with Layer 3 (%p) attached",m_layer3);
    attach(0);
    XDebug(this,DebugAll,"~ISDNLayer2");
}

// Attach an ISDN Q.931 Layer 3 if the given parameter is different from the one we have
void ISDNLayer2::attach(ISDNLayer3* layer3)
{
    Lock lock(m_layer3Mutex);
    if (m_layer3 == layer3)
	return;
    cleanup();
    ISDNLayer3* tmp = m_layer3;
    m_layer3 = layer3;
    lock.drop();
    if (tmp) {
	const char* name = 0;
	if (engine() && engine()->find(tmp)) {
	    name = tmp->toString().safe();
	    tmp->attach(0);
	}
	Debug(this,DebugAll,"Detached L3 (%p,'%s') [%p]",tmp,name,this);
    }
    if (!layer3)
	return;
    Debug(this,DebugAll,"Attached L3 (%p,'%s') [%p]",layer3,layer3->toString().safe(),this);
    insert(layer3);
    layer3->attach(this);
}

// Indication/confirmation of 'multiple frame acknoledged' mode established
void ISDNLayer2::multipleFrameEstablished(bool confirmation, bool timeout)
{
    Lock lock(m_layer3Mutex);
    if (m_layer3)
	m_layer3->multipleFrameEstablished(confirmation,timeout,this);
    else
	Debug(this,DebugNote,"'Established' notification. No Layer 3 attached");
}

// Indication/confirmation of 'multiple frame acknoledged' mode released
void ISDNLayer2::multipleFrameReleased(bool confirmation, bool timeout)
{
    Lock lock(m_layer3Mutex);
    if (m_layer3)
	m_layer3->multipleFrameReleased(confirmation,timeout,this);
    else
	Debug(this,DebugNote,"'Released' notification. No Layer 3 attached");
}

// Data link state change command/response
void ISDNLayer2::dataLinkState(bool cmd, bool value)
{
    Lock lock(m_layer3Mutex);
    if (m_layer3)
	m_layer3->dataLinkState(cmd,value,this);
    else
	Debug(this,DebugNote,"Data link notification. No Layer 3 attached");
}

// Notify layer 3 of data link idle timeout
void ISDNLayer2::idleTimeout()
{
    Lock lock(m_layer3Mutex);
    if (m_layer3)
	m_layer3->idleTimeout(this);
    else
	Debug(this,DebugNote,"Data link idle timeout. No Layer 3 attached");
}

// Indication of received data
void ISDNLayer2::receiveData(const DataBlock& data, bool ack)
{
    Lock lock(m_layer3Mutex);
    if (m_layer3)
	m_layer3->receiveData(data,ack,this);
    else
	Debug(this,DebugNote,"Data received. No Layer 3 attached");
}

// Change TEI ASSIGNED state
void ISDNLayer2::teiAssigned(bool status)
{
    Lock lock(m_interfaceMutex);
    if (m_teiAssigned == status)
	return;
    m_teiAssigned = status;
    XDebug(this,DebugAll,"%s 'TEI assigned' state",
	m_teiAssigned ? "Enter" : "Exit from");
    if (!m_teiAssigned)
	cleanup();
}

// Change the data link status while in TEI ASSIGNED state
void ISDNLayer2::changeState(State newState)
{
    Lock lock(m_interfaceMutex);
    if (!m_teiAssigned)
	return;
    if (m_state == newState)
	return;
    DDebug(this,DebugInfo,"Changing state from '%s' to '%s'",
	stateName(m_state),stateName(newState));
    m_state = newState;
}

// Change the interface type
bool ISDNLayer2::changeType()
{
    Lock lock(m_interfaceMutex);
    DDebug(this,DebugNote,"Interface type changed from '%s' to '%s'",
	linkSide(m_network),linkSide(!m_network));
    m_network = !m_network;
    return true;
}

/**
 * ISDNFrame
 */
// Flags used to set/get frame type
#define Q921FRAME_U                 0x03 // U frame
#define Q921FRAME_S                 0x01 // S frame
// U frame: P/F bit
#define Q921FRAME_U_GET_PF          0x10 // Mask to get bit 4: the P/F bit
#define Q921FRAME_U_RESET_PF        0xef // Mask to reset bit 4: the P/F bit
// Masks used to set/get command/response bits
#define Q921FRAME_CR_RR             0x01 // S frame
#define Q921FRAME_CR_UI             0x03 // U frame
#define Q921FRAME_CR_RNR            0x05 // S frame
#define Q921FRAME_CR_REJ            0x09 // S frame
#define Q921FRAME_CR_DM             0x0f // U frame
#define Q921FRAME_CR_DISC           0x43 // U frame
#define Q921FRAME_CR_FRMR           0x87 // U frame
#define Q921FRAME_CR_UA             0x63 // U frame
#define Q921FRAME_CR_SABME          0x6f // U frame
#define Q921FRAME_CR_XID            0xaf // U frame

// Set the address field of a frame header
// buf  Destination buffer
// cr   Command/response type
// network True if the sender is the network side of the data link
// sapi SAPI value
// tei  TEI value
static inline void setAddress(u_int8_t* buf, bool cr, bool network,
	u_int8_t sapi, u_int8_t tei)
{
    // Bit 0 is always 0. Set SAPI and C/R bit (bit 1)
    cr = cr ? ISDNFrame::commandBit(network) : ISDNFrame::responseBit(network);
    buf[0] = sapi << 2;
    if (cr)
	buf[0] |= 0x02;
    // Bit 1 is always 1. Set TEI
    buf[1] = (tei << 1) | 0x01;
}

// Set the control field of an U frame header
// buf  Destination buffer
// cr   Command/response value: P/F bit (bit 4) is 0
// pf   P/F bit
static inline void setControlU(u_int8_t* buf, u_int8_t cr, bool pf)
{
    if (pf)
	buf[2] = cr | Q921FRAME_U_GET_PF;
    else
	buf[2] = cr;
}

// Set the control field of an S or I frame header
// buf    Destination buffer
// cr_ns  S frame: Command/response value (P/F bit (bit 4) is 0)
//        I frame: N(S) value
// nr     N(R) value to set
// pf     P/F bit
static inline void setControl(u_int8_t* buf, u_int8_t cr_ns, u_int8_t nr, bool pf)
{
    buf[2] = cr_ns;
    buf[3] = nr << 1;
    if (pf)
	buf[3] |= 0x01;
}

TokenDict ISDNFrame::s_types[] = {
	{"DISC", DISC},
	{"DM", DM},
	{"FRMR", FRMR},
	{"I", I},
	{"REJ", REJ},
	{"RNR", RNR},
	{"RR", RR},
	{"SABME", SABME},
	{"UA", UA},
	{"UI", UI},
	{"XID", XID},
	{"Invalid frame", Invalid},
	{"Unknown command/response", ErrUnknownCR},
	{"Invalid header length", ErrHdrLength},
	{"Information field too long", ErrDataLength},
	{"Invalid N(R) (transmiter receive) sequence number", ErrRxSeqNo},
	{"Invalid N(S) (transmiter send) sequence number", ErrTxSeqNo},
	{"Invalid 'extended address' bit(s)", ErrInvalidEA},
	{"Invalid SAPI/TEI", ErrInvalidAddress},
	{"Unsupported command/response", ErrUnsupported},
	{"Invalid command/response flag", ErrInvalidCR},
	{0,0}
	};

// NOTE:
//   In constructors, the values of SAPI, TEI, N(S), N(R) are not checked to be in their interval:
//	this is done by the parser (when receiveing) and by ISDNLayer2 when assigning these values

// Constructs an undefined frame. Used by the parser
ISDNFrame::ISDNFrame(Type type)
    : m_type(type),
      m_error(type),
      m_category(Error),
      m_command(false),
      m_sapi(0),
      m_tei(0),
      m_poll(false),
      m_ns(0xFF),
      m_nr(0xFF),
      m_headerLength(0),
      m_dataLength(0),
      m_sent(false)
{
}

// Create U/S frames: SABME/DM/DISC/UA/FRMR/XID/RR/RNR/REJ
ISDNFrame::ISDNFrame(Type type, bool command, bool senderNetwork,
	u_int8_t sapi, u_int8_t tei, bool pf, u_int8_t nr)
    : m_type(type),
      m_error(type),
      m_category(Error),
      m_command(command),
      m_senderNetwork(senderNetwork),
      m_sapi(sapi),
      m_tei(tei),
      m_poll(pf),
      m_ns(0xFF),
      m_nr(nr),
      m_headerLength(3),
      m_dataLength(0),
      m_sent(false)
{
    u_int8_t buf[4];
    setAddress(buf,m_command,m_senderNetwork,m_sapi,m_tei);
    u_int8_t cr = 0;
#define Q921_CASE_SET_CRMASK(compare,rvalue,hdrLen,category) \
	case compare: cr = rvalue; m_headerLength = hdrLen; m_category = category; break;
    switch (m_type) {
	Q921_CASE_SET_CRMASK(SABME,Q921FRAME_CR_SABME,3,Unnumbered)
	Q921_CASE_SET_CRMASK(DM,Q921FRAME_CR_DM,3,Unnumbered)
	Q921_CASE_SET_CRMASK(DISC,Q921FRAME_CR_DISC,3,Unnumbered)
	Q921_CASE_SET_CRMASK(UA,Q921FRAME_CR_UA,3,Unnumbered)
	Q921_CASE_SET_CRMASK(FRMR,Q921FRAME_CR_FRMR,3,Unnumbered)
	Q921_CASE_SET_CRMASK(RR,Q921FRAME_CR_RR,4,Supervisory)
	Q921_CASE_SET_CRMASK(RNR,Q921FRAME_CR_RNR,4,Supervisory)
	Q921_CASE_SET_CRMASK(REJ,Q921FRAME_CR_REJ,4,Supervisory)
	Q921_CASE_SET_CRMASK(XID,Q921FRAME_CR_XID,3,Unnumbered)
	default:
	    return;
    }
#undef Q921_CASE_SET_CRMASK
    // Set control field
    if (m_headerLength == 3)
	setControlU(buf,cr,m_poll);
    else
	setControl(buf,cr,m_nr,m_poll);
    // Set frame buffer
    m_buffer.assign(buf,m_headerLength);
}

// Create I/UI frames
ISDNFrame::ISDNFrame(bool ack, bool senderNetwork, u_int8_t sapi, u_int8_t tei,
	bool pf, const DataBlock& data)
    : m_type(I),
      m_error(I),
      m_category(Data),
      m_command(true),
      m_senderNetwork(senderNetwork),
      m_sapi(sapi),
      m_tei(tei),
      m_poll(pf),
      m_ns(0),
      m_nr(0),
      m_headerLength(4),
      m_dataLength(data.length()),
      m_sent(false)
{
    if (!ack) {
	m_type = m_error = UI;
	m_headerLength = 3;
	m_ns = m_nr = 0xff;
    }
    u_int8_t buf[4];
    setAddress(buf,m_command,m_senderNetwork,m_sapi,m_tei);
    if (m_type == I)
	setControl(buf,m_ns << 1,m_nr << 1,m_poll);
    else
	setControlU(buf,Q921FRAME_CR_UI,m_poll);
    m_buffer.assign(buf,m_headerLength);
    m_buffer += data;
}

ISDNFrame::~ISDNFrame()
{
}

// Update transmitter send and transmitter receive values for I (data) frames
void ISDNFrame::update(u_int8_t* ns, u_int8_t* nr)
{
#define NS (((u_int8_t*)(m_buffer.data()))[2])
#define NR (((u_int8_t*)(m_buffer.data()))[3])
    if (m_type != I)
	return;
    if (ns) {
	m_ns = *ns;
	// For I frames bit 0 of N(S) is always 0
	NS = m_ns << 1;
    }
    if (nr) {
	m_nr = *nr;
	// Keep the P/F bit (bit 0)
	NR = (m_nr << 1) | (NR & 0x01);
    }
#undef NR
#undef NS
}

// Put the frame in a string for debug purposes
void ISDNFrame::toString(String& dest, bool extendedDebug) const
{
#define STARTLINE(indent) "\r\n" << indent
    const char* enclose = "\r\n-----";
    const char* ind = "  ";
    dest << enclose;
    dest << STARTLINE("") << name();
    // Dump header
    if (extendedDebug) {
	String tmp;
	tmp.hexify((void*)buffer().data(),headerLength(),' ');
	dest << " - Header dump: " << tmp;
    }
    if (m_error >= Invalid)
	dest << STARTLINE(ind) << "Error: " << typeName(m_error);
    // Address
    dest << STARTLINE(ind) << "SAPI=" << (unsigned int)m_sapi;
    dest << "  TEI=" << (unsigned int)m_tei;
    dest << "  Type=" << (m_command ? "Command" : "Response");
    // Control
    dest << "  Poll/Final=" << (m_poll ? '1' : '0');
    dest << "  Sequence numbers: ";
    switch (m_type) {
	case I:
	    dest << "Send=" << (unsigned int)m_ns;
	    dest << " Recv=" << (unsigned int)m_nr;
	    break;
	case RR:
	case RNR:
	case REJ:
	    dest << "Send=N/A Recv=" << (unsigned int)m_nr;
	    break;
	default: ;
	    dest << "Send=N/A Recv=N/A";
    }
    // Data
    dest << STARTLINE(ind) << "Retransmission=" << String::boolText(m_sent);
    dest << "  Length: Header=" << (unsigned int)m_headerLength;
    dest << " Data=" << (unsigned int)m_dataLength;
    // Dump data
    if (extendedDebug && m_dataLength) {
	String tmp;
	tmp.hexify((char*)buffer().data() + headerLength(),m_dataLength,' ');
        dest << STARTLINE(ind) << "Data dump: " << tmp;
    }
    dest << enclose;
#undef STARTLINE
}

// Parse received buffer. Set frame data. Header description:
// Address: 2 bytes
// Control: 1 or 2 bytes
// Data: Variable
//
// Address field: 2 bytes (1 and 2)
//    Check EA bits: bit 0 of byte 0 must be 0; bit 0 of byte 1 must be 1
//    C/R (command/response) bit: bit 1 of byte 0
//    SAPI: Bits 2-7 of byte 0
//    TEI: Bits 1-7 of byte 1
// Control field: 1 byte (byte 2) for U frames and 2 bytes (bytes 2 and 3) for I/S frames
//     Frame type: Bits 0,1 of of byte 2
//     P/F (Poll/Final) bit: I/S frame: bit 0 of byte 3. U frame: bit 4 of the byte 2
//     Command/response code: I frame: none. S frame: byte 2. U frame: byte 2 with P/F bit reset
ISDNFrame* ISDNFrame::parse(const DataBlock& data, ISDNLayer2* receiver)
{
    // We MUST have 2 bytes for address and at least 1 byte for control field
    if (!receiver || data.length() < 3)
	return 0;
    ISDNFrame* frame = new ISDNFrame(Invalid);
    const u_int8_t* buf = (const u_int8_t*)(data.data());
    // *** Address field: 2 bytes
    // Check EA bits
    if ((buf[0] & 0x01) || !(buf[1] & 0x01)) {
	frame->m_buffer = data;
	frame->m_headerLength = frame->m_buffer.length();
	frame->m_error = ErrInvalidEA;
	return frame;
    }
    // Get C/R bit, SAPI, TEI
    // C/R: (Q.921 Table 1):
    //   network --> user      Command: 1   Response: 0
    //   user    --> network   Command: 0   Response: 1
    // The sender of this frame is the other side of the receiver
    frame->m_senderNetwork = !receiver->network();
    frame->m_command = isCommand(buf[0] & 0x02,frame->m_senderNetwork);
    frame->m_sapi = buf[0] >> 2;
    frame->m_tei = buf[1] >> 1;
    // *** Control field: 1 (U frame) or 2 (I/S frame) bytes
    // Get frame type: I/U/S. I/S frame type control field is 2 bytes long
    u_int8_t type = buf[2] & 0x03;
    if (type != Q921FRAME_U && data.length() < 4) {
	frame->m_buffer = data;
	frame->m_headerLength = 3;
	frame->m_error = ErrHdrLength;
	return frame;
    }
    // Adjust frame header length. Get P/F bit
    // Get counters. Set frame type
#define Q921_CASE_SETTYPE(compare,rvalue,category)\
	case compare: frame->m_type = frame->m_error = rvalue; frame->m_category = category; break;
    switch (type) {
	case Q921FRAME_U:
	    frame->m_headerLength = 3;
	    frame->m_poll = (buf[2] & Q921FRAME_U_GET_PF) ? true : false;
	    switch (buf[2] & Q921FRAME_U_RESET_PF) {
		Q921_CASE_SETTYPE(Q921FRAME_CR_UA,UA,Unnumbered)
		Q921_CASE_SETTYPE(Q921FRAME_CR_DM,DM,Unnumbered)
		Q921_CASE_SETTYPE(Q921FRAME_CR_DISC,DISC,Unnumbered)
		Q921_CASE_SETTYPE(Q921FRAME_CR_SABME,SABME,Unnumbered)
		Q921_CASE_SETTYPE(Q921FRAME_CR_UI,UI,Data)
		Q921_CASE_SETTYPE(Q921FRAME_CR_FRMR,FRMR,Unnumbered)
		Q921_CASE_SETTYPE(Q921FRAME_CR_XID,XID,Unnumbered)
		default:
		    frame->m_type = Invalid;
		    frame->m_error = ErrUnknownCR;
	    }
	    break;
	case Q921FRAME_S:
	    frame->m_headerLength = 4;
	    frame->m_poll = (buf[3] & 0x01) ? true : false;
	    frame->m_nr = buf[3] >> 1;
	    switch (buf[2]) {
		Q921_CASE_SETTYPE(Q921FRAME_CR_RR,RR,Supervisory)
		Q921_CASE_SETTYPE(Q921FRAME_CR_RNR,RNR,Supervisory)
		Q921_CASE_SETTYPE(Q921FRAME_CR_REJ,REJ,Supervisory)
		default:
		    frame->m_type = Invalid;
		    frame->m_error = ErrUnknownCR;
	    }
	    break;
	default:            // I frame
	    frame->m_type = frame->m_error = I;
	    frame->m_category = Data;
	    frame->m_headerLength = 4;
	    frame->m_poll = (buf[3] & 0x01) ? true : false;
	    frame->m_ns = buf[2] >> 1;
	    frame->m_nr = buf[3] >> 1;
    }
#undef Q921_CASE_SETTYPE
    // Copy buffer. Set data length
    frame->m_buffer = data;
    frame->m_dataLength = data.length() - frame->m_headerLength;
    return frame;
}

/* vi: set ts=8 sw=4 sts=4 noet: */