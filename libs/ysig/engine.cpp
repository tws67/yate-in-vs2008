/**
 * engine.cpp
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
#include <yateversn.h>

#include <string.h>


namespace TelEngine {

class SignallingThreadPrivate : public Thread
{
public:
    inline SignallingThreadPrivate(SignallingEngine* engine, const char* name, Priority prio, unsigned long usec)
	: Thread(name,prio), m_engine(engine), m_sleep(usec)
	{ }
    virtual ~SignallingThreadPrivate();
    virtual void run();

private:
    SignallingEngine* m_engine;
    unsigned long m_sleep;
};

};


using namespace TelEngine;

static ObjList s_factories;
static Mutex s_mutex(true);

SignallingFactory::SignallingFactory()
{
    s_mutex.lock();
    s_factories.append(this)->setDelete(false);
    s_mutex.unlock();
}

SignallingFactory::~SignallingFactory()
{
    s_mutex.lock();
    s_factories.remove(this,false);
    s_mutex.unlock();
}

void* SignallingFactory::build(const String& type, const NamedList* name)
{
    if (type.null())
	return 0;
    NamedList dummy(type);
    if (!name)
	name = &dummy;
    Lock lock(s_mutex);
    for (ObjList* l = &s_factories; l; l = l->next()) {
	SignallingFactory* f = static_cast<SignallingFactory*>(l->get());
	if (!f)
	    continue;
	XDebug(DebugAll,"Attempting to create a %s %s using factory %p",
	    name->c_str(),type.c_str(),f);
	void* obj = f->create(type,*name);
	if (obj)
	    return obj;
    }
    lock.drop();
    // now build some objects we know about
    if (type == "SignallingEngine")
	return new SignallingEngine;
    else if (type == "SS7MTP2")
	return new SS7MTP2(*name);
    else if (type == "SS7MTP3")
	return new SS7MTP3(*name);
    else if (type == "SS7Router")
	return new SS7Router(*name);
    else return 0;
}


void SignallingComponent::setName(const char* name)
{
    debugName(0);
    m_name = name;
    debugName(m_name);
}

SignallingComponent::~SignallingComponent()
{
    DDebug(engine(),DebugAll,"Component '%s' deleted [%p]",toString().c_str(),this);
    detach();
}

const String& SignallingComponent::toString() const
{
    return m_name;
}

void SignallingComponent::insert(SignallingComponent* component)
{
    if (!component)
	return;
    if (m_engine) {
	// we have an engine - force the other component in the same
	m_engine->insert(component);
	return;
    }
    if (component->engine())
	// insert ourselves in the other's engine
	component->engine()->insert(this);
}

void SignallingComponent::detach()
{
    debugChain();
    if (m_engine) {
	m_engine->remove(this);
	m_engine = 0;
    }
}

void SignallingComponent::timerTick(const Time& when)
{
    XDebug(engine(),DebugAll,"Timer ticked for component '%s' [%p]",
	toString().c_str(),this);
}


SignallingEngine::SignallingEngine(const char* name)
    : Mutex(true), m_thread(0), m_listChanged(true)
{
    debugName(name);
}

SignallingEngine::~SignallingEngine()
{
    if (m_thread) {
	Debug(this,DebugGoOn,
	    "Engine destroyed with worker thread still running [%p]",this);
	stop();
    }
    lock();
    m_components.clear();
    unlock();
}

SignallingComponent* SignallingEngine::find(const String& name)
{
    Lock lock(this);
    return static_cast<SignallingComponent*>(m_components[name]);
}

bool SignallingEngine::find(const SignallingComponent* component)
{
    if (!component)
	return false;
    Lock lock(this);
    return m_components.find(component) != 0;
}

void SignallingEngine::insert(SignallingComponent* component)
{
    if (!component)
	return;
    if (component->engine() == this)
	return;
    Lock lock(this);
    DDebug(this,DebugAll,"Engine inserting component '%s' @%p [%p]",
	component->toString().c_str(),component,this);
    component->detach();
    component->m_engine = this;
    component->debugChain(this);
    m_components.append(component);
}

void SignallingEngine::remove(SignallingComponent* component)
{
    if (!component)
	return;
    if (component->engine() != this)
	return;
    Lock lock(this);
    DDebug(this,DebugAll,"Engine removing component '%s' @%p [%p]",
	component->toString().c_str(),component,this);
    component->m_engine = 0;
    component->detach();
    m_components.remove(component,false);
}

bool SignallingEngine::remove(const String& name)
{
    if (name.null())
	return false;
    Lock lock(this);
    SignallingComponent* component = find(name);
    if (!component)
	return false;
    DDebug(this,DebugAll,"Engine removing component '%s' @%p [%p]",
	component->toString().c_str(),component,this);
    component->m_engine = 0;
    component->detach();
    m_components.remove(component);
    return true;
}

bool SignallingEngine::start(const char* name, Thread::Priority prio, unsigned long usec)
{
    Lock lock(this);
    if (m_thread)
	return m_thread->running();
    // sanity check - 20ms is long enough
    if (usec > 20000)
	usec = 20000;

    // TODO: experimental: remove commented if it's working
    m_thread = new SignallingThreadPrivate(this,name,prio,usec);
    if (m_thread->startup()) {
	Debug(this,DebugAll,"Engine started worker thread [%p]",this);
	return true;
    }

#if 0
    SignallingThreadPrivate* tmp = new SignallingThreadPrivate(this,name,prio,usec);
    if (tmp->startup()) {
	m_thread = tmp;
	DDebug(this,DebugInfo,"Engine started worker thread [%p]",this);
	return true;
    }
    delete tmp;
#endif
    Debug(this,DebugGoOn,"Engine failed to start worker thread [%p]",this);
    return false;
}

void SignallingEngine::stop()
{
    // TODO: experimental: remove commented if it's working
    if (!m_thread)
	return;
    m_thread->cancel(false);
    while (m_thread)
	Thread::yield(true);
    Debug(this,DebugAll,"Engine stopped worker thread [%p]",this);
#if 0
    lock();
    SignallingThreadPrivate* tmp = m_thread;
    m_thread = 0;
    if (tmp) {
	delete tmp;
	DDebug(this,DebugInfo,"Engine stopped worker thread [%p]",this);
    }
    unlock();
#endif
}

Thread* SignallingEngine::thread() const
{
    return m_thread;
}

void SignallingEngine::timerTick(const Time& when)
{
    lock();
    m_listChanged = false;
    for (ObjList* l = &m_components; l; l = l->next()) {
	SignallingComponent* c = static_cast<SignallingComponent*>(l->get());
	if (c) {
	    c->timerTick(when);
	    // if the list was changed (can be only from this thread) we
	    //  break out and get back later - cheaper than using a ListIterator
	    if (m_listChanged)
		break;
	}
    }
    unlock();
}


SignallingThreadPrivate::~SignallingThreadPrivate()
{
    if (m_engine)
	m_engine->m_thread = 0;
}

void SignallingThreadPrivate::run()
{
    for (;;) {
	if (m_engine) {
	    Time t;
	    m_engine->timerTick(t);
	    if (m_sleep) {
		usleep(m_sleep,true);
		continue;
	    }
	}
	yield(true);
    }
}


/**
 * SignallingUtils
 */

// Coding standard as defined in Q.931/Q.850
static TokenDict s_dict_codingStandard[] = {
	{"CCITT",            0x00},
	{"ISO/IEC",          0x20},
	{"national",         0x40},
	{"network specific", 0x50},
	{0,0}
	};

// Locations as defined in Q.850
static TokenDict s_dict_location[] = {
	{"U",    0x00},                  // User
	{"LPN",  0x01},                  // Private network serving the local user
	{"LN",   0x02},                  // Public network serving the local user
	{"TN",   0x03},                  // Transit network
	{"RLN",  0x04},                  // Public network serving the remote user
	{"RPN",  0x05},                  // Private network serving the remote user
	{"INTL", 0x07},                  // International network
	{"BI",   0x0a},                  // Network beyond the interworking point
	{0,0}
	};

// Q.850 2.2.5. Cause class: Bits 4-6
// Q.850 Table 1. Cause value: Bits 0-6
// Defined for CCITT coding standard
static TokenDict s_dict_causeCCITT[] = {
	// normal-event class
	{"normal-event",                   0x00},
	{"unallocated",                    0x01}, // Unallocated (unassigned) number
	{"noroute-to-network",             0x02}, // No route to specified transit network
	{"noroute",                        0x03}, // No route to destination
	{"channel-unacceptable",           0x06}, // Channel unacceptable
	{"call-delivered",                 0x07}, // Call awarded and being delivered in an established channel
	{"normal-clearing",                0x10}, // Normal Clearing
	{"busy",                           0x11}, // User busy
	{"noresponse",                     0x12}, // No user responding
	{"noanswer",                       0x13}, // No answer from user (user alerted)
	{"rejected",                       0x15}, // Call Rejected
	{"moved",                          0x16}, // Number changed
	{"non-sel-user-clearing",          0x1a}, // Non-selected user clearing
	{"offline",                        0x1b}, // Destination out of order
	{"invalid-number",                 0x1c}, // Invalid number format
	{"facility-rejected",              0x1d}, // Facility rejected
	{"status-enquiry-rsp",             0x1e}, // Response to STATUS ENQUIRY
	{"normal",                         0x1f}, // Normal, unspecified
	// resource-unavailable class
	{"resource-unavailable",           0x20}, // Resource unavailable
	{"congestion",                     0x22}, // No circuit/channel available
	{"net-out-of-order",               0x26}, // Network out of order
	{"temporary-failure",              0x29}, // Temporary failure
	{"congestion",                     0x2a}, // Switching equipment congestion
	{"access-info-discarded",          0x2b}, // Access information discarded
	{"noconn",                         0x2c}, // Requested channel not available
	{"noresource",                     0x2f}, // Resource unavailable, unspecified
	{"service-unavailable",            0x30}, // Service or option not available
	{"qos-unavailable",                0x31}, // Quality of service unavailable
	{"facility-not-subscribed",        0x32}, // Requested facility not subscribed
	{"forbidden-out",                  0x35}, // Outgoing call barred within CUG
	{"forbidden-in",                   0x37}, // Incoming call barred within CUG
	{"bearer-cap-not-auth",            0x39}, // Bearer capability not authorized
	{"bearer-cap-not-available",       0x3a}, // Bearer capability not presently available
	{"service-unavailable",            0x3f}, // Service or option not available
	// service-not-implemented class
	{"bearer-cap-not-implemented",     0x41}, // Bearer capability not implemented
	{"channel-type-not-implemented",   0x42}, // Channel type not implemented
	{"facility-not-implemented",       0x45}, // Requested facility not implemented
	{"restrict-bearer-cap-avail",      0x46}, // Only restricted digital information bearer capability is available
	{"service-not-implemented",        0x4f}, // Service or option not implemented, unspecified
	// invalid-message class
	{"invalid-callref",                0x51}, // Invalid call reference value
	{"unknown-channel",                0x52}, // Identified channel does not exist
	{"unknown-callid",                 0x53}, // A suspended call exists, but this call identity does not
	{"duplicate-callid",               0x54}, // Call identity in use
	{"no-call-suspended",              0x55}, // No call suspended
	{"suspended-call-cleared",         0x56}, // Call having the requested call identity has been cleared
	{"incompatible-dest",              0x58}, // Incompatible destination
	{"invalid-message",                0x5f}, // Invalid message, unspecified
	// protocol-error class 
	{"missing-mandatory-ie",           0x60}, // Mandatory information element is missing
	{"unknown-message",                0x61}, // Message type non-existent or not implemented
	{"wrong-message",                  0x62}, // Message not compatible with call state, non-existent or not implemented
	{"unknown-ie",                     0x63}, // Information element non-existent or not implemented
	{"invalid-ie",                     0x64}, // Invalid information element contents
	{"wrong-state-message",            0x65}, // Message not compatible with call state
	{"timeout",                        0x66}, // Recovery on timer expiry
	{"protocol-error",                 0x6f}, // Protocol error, unspecified
	// interworking class
	{"interworking",                   0x7f}, // Interworking, unspecified
	{0,0}
	};

// Q.931 4.5.5. Information transfer capability: Bits 0-4
// Defined for CCITT coding standard
static TokenDict s_dict_transferCapCCITT[] = {
	{"speech",       0x00},          // Speech
	{"udi",          0x08},          // Unrestricted digital information
	{"rdi",          0x09},          // Restricted digital information
	{"3.1khz-audio", 0x10},          // 3.1 khz audio
	{"udi-ta",       0x11},          // Unrestricted digital information with tone/announcements
	{"video",        0x18},          // Video
	{0,0}
	};

// Q.931 4.5.5. Transfer mode: Bits 5,6
// Defined for CCITT coding standard
static TokenDict s_dict_transferModeCCITT[] = {
	{"circuit",      0x00},          // Circuit switch mode
	{"packet",       0x40},          // Packet mode
	{0,0}
	};

// Q.931 4.5.5. Transfer rate: Bits 0-4
// Defined for CCITT coding standard
static TokenDict s_dict_transferRateCCITT[] = {
	{"packet",        0x00},         // Packet mode only
	{"64kbit",        0x10},         // 64 kbit/s
	{"2x64kbit",      0x11},         // 2x64 kbit/s
	{"384kbit",       0x13},         // 384 kbit/s
	{"1536kbit",      0x15},         // 1536 kbit/s
	{"1920kbit",      0x17},         // 1920 kbit/s
	{"multirate",     0x18},         // Multirate (64 kbit/s base rate)
	{0,0}
	};

// Q.931 4.5.5. User information Layer 1 protocol: Bits 0-4
// Defined for CCITT coding standard
static TokenDict s_dict_formatCCITT[] = {
	{"v110",          0x01},         // Recomendation V.110 and X.30
	{"mulaw",         0x02},         // Recomendation G.711 mu-law
	{"alaw",          0x03},         // Recomendation G.711 A-law 
	{"g721",          0x04},         // Recomendation G.721 32kbit/s ADPCM and I.460
	{"h221",          0x05},         // Recomendation H.221 and H.242
	{"non-CCITT",     0x07},         // Non CCITT standardized rate adaption
	{"v120",          0x08},         // Recomendation V.120
	{"x31",           0x09},         // Recomendation X.31 HDLC flag stuffing
	{0,0}
	};

TokenDict* SignallingUtils::s_dictCCITT[5] = {
	s_dict_causeCCITT,
	s_dict_formatCCITT,
	s_dict_transferCapCCITT,
	s_dict_transferModeCCITT,
	s_dict_transferRateCCITT
	};

// Check if a list's parameter (comma separated list of flags) has a given flag
bool SignallingUtils::hasFlag(const NamedList& list, const char* param, const char* flag)
{
    String s = list.getValue(param);
    ObjList* obj = s.split(',',false);
    bool found = (obj->find(flag) != 0);
    TelEngine::destruct(obj);
    return found;
}

// Remove a flag from a comma separated list of flags
bool SignallingUtils::removeFlag(String& flags, const char* flag)
{
    ObjList* obj = flags.split(',',false);
    ObjList* found = obj->find(flag);
    if (found) {
	obj->remove(found,true);
	flags = "";
	for (ObjList* o = obj->skipNull(); o; o = o->skipNext())
	    flags.append(*static_cast<String*>(o->get()),",");
    }
    TelEngine::destruct(obj);
    return (found != 0);
}

// Add string (keyword) if found or integer parameter to a named list
void SignallingUtils::addKeyword(NamedList& list, const char* param, const TokenDict* tokens, unsigned int val)
{
    const char* value = lookup(val,tokens);
    if (value)
	list.addParam(param,value);
    else
	list.addParam(param,String(val));
}

// Dump a buffer to a list of parameters
void SignallingUtils::dumpData(const SignallingComponent* comp, NamedList& list, const char* param,
	const unsigned char* buf, unsigned int len, char sep)
{
    String raw;
    raw.hexify((void*)buf,len,sep);
    list.addParam(param,raw);
    DDebug(comp,DebugAll,"Utils::dumpData dumped %s='%s'",param,raw.safe());
}

// Dump data from a buffer to a list of parameters. The buffer is parsed until (and including)
//  the first byte with the extension bit (the most significant one) set
unsigned int SignallingUtils::dumpDataExt(const SignallingComponent* comp, NamedList& list, const char* param,
	const unsigned char* buf, unsigned int len, char sep)
{
    if (!(buf && len))
	return 0;
    unsigned int count = 0;
    for (; count < len && !(buf[count] & 0x80); count++) ;
    if (count == len) {
	Debug(comp,DebugMild,"Utils::dumpDataExt invalid ext bits for %s (len=%u)",param,len);
	return 0;
    }
    dumpData(comp,list,param,buf,count,sep);
    return count;
}

// Decode a received buffer to a comma separated list of flags
bool SignallingUtils::decodeFlags(const SignallingComponent* comp, NamedList& list, const char* param,
	const SignallingFlags* flags, const unsigned char* buf, unsigned int len)
{
    if (!(flags && buf && len <= sizeof(unsigned int)))
	return false;
    unsigned int val = 0;
    int shift = 0;
    while (len--) {
	val |= ((unsigned int)*buf++) << shift;
	shift += 8;
    }
    String tmp;
    for (; flags->mask; flags++)
	if ((val & flags->mask) == flags->value)
	    tmp.append(flags->name,",");
    DDebug(comp,DebugAll,"Utils::decodeFlags. Decoded %s='%s' from %u",param,tmp.safe(),val);
    list.addParam(param,tmp);
    return true;
}

const TokenDict* SignallingUtils::codings()
{
    return s_dict_codingStandard;
}

const TokenDict* SignallingUtils::locations()
{
    return s_dict_location;
}

#define Q850_MAX_CAUSE 32

// Q.850 2.1
bool SignallingUtils::decodeCause(const SignallingComponent* comp, NamedList& list,
	const unsigned char* buf, unsigned int len, const char* prefix, bool isup)
{
    if (!buf)
	return false;
    if (len < 2) {
	Debug(comp,DebugNote,"Utils::decodeCause. Invalid length %u",len);
	return false;
    }
    String causeName = prefix;
    // Byte 0: Coding standard (bit 5,6), location (bit 0-3)
    unsigned char coding = buf[0] & 0x60;
    addKeyword(list,causeName + ".coding",codings(),coding);
    addKeyword(list,causeName + ".location",locations(),buf[0] & 0x0f);
    unsigned int crt = 1;
    // If bit 7 is 0, the next byte should contain the recomendation
    unsigned char rec = 0;
    if (!(buf[0] & 0x80)) {
	rec = buf[1] & 0x7f;
	// For ISUP there shouldn't be a recomendation byte
	if (isup)
	    Debug(comp,DebugMild,"Utils::decodeCause. Found recomendation %u for ISUP cause",rec);
	crt = 2;
    }
    if (rec)
	list.addParam(causeName + ".rec",String(rec));
    if (crt >= len) {
	Debug(comp,DebugMild,"Utils::decodeCause. Invalid length %u. Cause value is missing",len);
	list.addParam(causeName,"");
	return false;
    }
    // Current byte: bits 0..6: cause, bits 5,6: cause class
    addKeyword(list,causeName,dict(0,coding),buf[crt] & 0x7f);
    // Rest of data: diagnostic
    crt++;
    if (crt < len)
	dumpData(comp,list,causeName + ".diagnostic",buf + crt,len - crt);
    return true;
}

// Decode bearer capabilities as defined in Q.931 (Bearer Capabilities) and Q.763 (User Service Information)
// Q.931 - 4.5.5 / Q.763 - 3.57
// The given sections in comments are from Q.931
bool SignallingUtils::decodeCaps(const SignallingComponent* comp, NamedList& list, const unsigned char* buf,
	unsigned int len, const char* prefix, bool isup)
{
    if (!buf)
	return false;
    if (len < 2) {
	Debug(comp,DebugMild,"Utils::decodeCaps. Invalid length %u",len);
	return false;
    }
    String capsName = prefix;
    // Byte 0: Coding standard (bit 5,6), Information transfer capability (bit 0-4)
    // Byte 1: Transfer mode (bit 5,6), Transfer rate (bit 0-4)
    unsigned char coding = buf[0] & 0x60;
    addKeyword(list,capsName + ".coding",codings(),coding);
    addKeyword(list,capsName + ".transfercap",dict(2,coding),buf[0] & 0x1f);
    addKeyword(list,capsName + ".transfermode",dict(3,coding),buf[1] & 0x60);
    u_int8_t rate = buf[1] & 0x1f;
    addKeyword(list,capsName + ".transferrate",dict(4,coding),rate);
    // Figure 4.11 Note 1: Next byte is the rate multiplier if the transfer rate is 'multirate' (0x18)
    u_int8_t crt = 2;
    if (rate == 0x18) {
	if (len < 3) {
	    Debug(comp,DebugMild,"Utils::decodeCaps. Invalid length %u. No rate multiplier",len);
	    return false;
	}
	addKeyword(list,capsName + ".multiplier",0,buf[2] & 0x7f);
	crt = 3;
    }
    // Get optional extra information
    // Layer 1 data
    if (len <= crt)
	return true;
    u_int8_t ident = (buf[crt] & 0x60) >> 5;
    if (ident != 1) {
	Debug(comp,DebugNote,"Utils::decodeCaps. Invalid layer 1 ident %u",ident);
	return true;
    }
    addKeyword(list,capsName,dict(1,coding),buf[crt] & 0x1f);
    //TODO: Decode the rest of Layer 1, Layer 2 and Layer 3 data
    return true;
}

// Encode a comma separated list of flags. Flags can be prefixed with the '-'
//  character to be reset if previously set
void SignallingUtils::encodeFlags(const SignallingComponent* comp,
	int& dest, const String& flags, TokenDict* dict)
{
    if (flags.null() || !dict)
	return;
    ObjList* list = flags.split(',',false);
    DDebug(comp,DebugAll,"Utils::encodeFlags '%s' dest=0x%x",flags.c_str(),dest);
    for (ObjList* o = list->skipNull(); o; o = o->skipNext()) {
	String* s = static_cast<String*>(o->get());
	bool set = !s->startSkip("-",false);
	TokenDict* p = dict;
	for (; p->token && *s != p->token; p++) ;
	if (!p->token) {
	    DDebug(comp,DebugAll,"Utils::encodeFlags '%s' not found",s->c_str());
	    continue;
	}
	DDebug(comp,DebugAll,"Utils::encodeFlags %sset %s=0x%x",
	    set?"":"re",p->token,p->value);
	if (set)
	    dest |= p->value;
	else
	    dest &= ~p->value;
    }
    TelEngine::destruct(list);
}

// Q.850 2.1
bool SignallingUtils::encodeCause(const SignallingComponent* comp, DataBlock& buf,
	const NamedList& params, const char* prefix, bool isup, bool fail)
{
    u_int8_t data[4] = {2,0x80,0x80,0x80};
    String causeName = prefix;
    // Coding standard (0: CCITT) + location. If no location, set it to 0x0a: "BI"
    unsigned char coding = (unsigned char)params.getIntValue(causeName + ".coding",codings(),0);
    unsigned char loc = (unsigned char)params.getIntValue(causeName + ".location",locations(),0x0a);
    data[1] |= ((coding & 0x03) << 5) | (loc & 0x0f);
    // Recommendation (only for Q.931)
    if (!isup) {
	unsigned char rec = (unsigned char)params.getIntValue(causeName + ".rec",0,0);
	// Add recommendation. Clear bit 7 of the first byte
	data[1] &= 0x7f;
	data[2] |= (rec & 0x7f);
	data[0] = 3;
    }
    // Value. Set to normal-clearing if missing for CCITT encoding or
    //  to 0 for other encoding standards
    unsigned char val = 0;
    if (!coding)
	val = (unsigned char)params.getIntValue(causeName,dict(0,0),0x10);
    data[data[0]] |= (val & 0x7f);
    // Diagnostic
    DataBlock diagnostic;
    const char* tmp = params.getValue(causeName + ".diagnostic");
    if (tmp)
	diagnostic.unHexify(tmp,strlen(tmp),' ');
    // Set data
    if (!isup && diagnostic.length() + data[0] + 1 > 32) {
	Debug(comp,fail?DebugNote:DebugMild,
	    "Utils::encodeCause. Cause length %u > 32. %s",
	    diagnostic.length() + data[0] + 1,fail?"Fail":"Skipping diagnostic");
	if (fail)
	    return false;
	diagnostic.clear();
    }
    data[0] += diagnostic.length();
    buf.assign(data,data[0] + 1);
    buf += diagnostic;
    return true;
}

bool SignallingUtils::encodeCaps(const SignallingComponent* comp, DataBlock& buf, const NamedList& params,
	const char* prefix, bool isup)
{
    u_int8_t data[5] = {2,0x80,0x80,0x80,0x80};
    String capsName = prefix;
    // Byte 1: Coding standard (bit 5,6), Information transfer capability (bit 0-4)
    // Byte 2: Transfer mode (bit 5,6), Transfer rate (bit 0-4)
    unsigned char coding = (unsigned char)params.getIntValue(capsName + ".coding",codings(),0);
    unsigned char cap = (unsigned char)params.getIntValue(capsName + ".transfercap",dict(2,coding),0);
    unsigned char mode = (unsigned char)params.getIntValue(capsName + ".transfermode",dict(3,coding),0);
    unsigned char rate = (unsigned char)params.getIntValue(capsName + ".transferrate",dict(4,coding),0x10);
    data[1] |= ((coding << 5) & 0x60) | (cap & 0x1f);
    data[2] |= ((mode << 5) & 0x60) | (rate & 0x1f);
    if (rate == 0x18) {
	data[0] = 3;
	rate = (unsigned char)params.getIntValue(capsName + ".multiplier",0,0);
	data[3] |= rate & 0x7f;
    }
    // User information layer data
    // Bit 7 = 1, Bits 5,6 = layer (1), Bits 0-4: the value
    int format = params.getIntValue(capsName,dict(1,coding),-1);
    if (format != -1) {
	data[data[0] + 1] |= 0x20 | (((unsigned char)format) & 0x1f);
	data[0]++;
    }
    buf.assign(data,data[0] + 1);
    return true;
}

// Parse a list of integers or integer intervals. Source elements must be separated by a
//   '.' or ',' character. Integer intervals must be separated by a '-' character.
// Empty elements are silently discarded
unsigned int* SignallingUtils::parseUIntArray(const String& source,
	unsigned int min, unsigned int max,
	unsigned int& count, bool discardDup)
{
    count = 0;
    ObjList* list = source.split(((-1!=source.find(','))?',':'.'),false);
    if (!list->count()) {
	TelEngine::destruct(list);
	return 0;
    }

    unsigned int maxArray = 0;
    unsigned int* array = 0;
    bool ok = true;
    int first, last;

    for (ObjList* o = list->skipNull(); o; o = o->skipNext()) {
	String* s = static_cast<String*>(o->get());
	// Get the interval (may be a single value)
	int sep = s->find('-');
	if (sep == -1)
	    first = last = s->toInteger(-1);
	else {
	    first = s->substr(0,sep).toInteger(-1);
	    last = s->substr(sep + 1).toInteger(-2);
	}
	if (first < 0 || last < 0 || last < first) {
	    ok = false;
	    break;
	}
	// Resize and copy array if not enough room
	unsigned int len = (unsigned int)(last - first + 1);
	if (count + len > maxArray) {
	    maxArray = count + len;
	    unsigned int* tmp = new unsigned int[maxArray];
	    if (array) {
		::memcpy(tmp,array,sizeof(unsigned int) * count);
		delete[] array;
	    }
	    array = tmp;
	}
	// Add to array code list
	for (; first <= last; first++) {
	    // Check interval
	    if ((unsigned int)first < min || max < (unsigned int)first) {
		ok = false;
		break;
	    }
	    // Check duplicates
	    if (discardDup) {
		bool dup = false;
		for (unsigned int i = 0; i < count; i++)
		    if (array[i] == (unsigned int)first) {
			dup = true;
			break;
		    }
		if (dup)
		    continue;
	    }
	    array[count++] = first;
	}
	if (!ok)
	    break;
    }
    TelEngine::destruct(list);

    if (ok && count)
	return array;
    count = 0;
    if (array)
	delete[] array;
    return 0;
}

/* vi: set ts=8 sw=4 sts=4 noet: */
