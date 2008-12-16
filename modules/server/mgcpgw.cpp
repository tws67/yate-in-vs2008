/**
 * mgcpgw.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Media Gateway Control Protocol - Gateway component
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


#include <yatephone.h>
#include <yatemgcp.h>

#include <stdlib.h>

using namespace TelEngine;
namespace { // anonymous

class YMGCPEngine : public MGCPEngine
{
public:
    inline YMGCPEngine(const NamedList* params)
	: MGCPEngine(true,0,params)
	{ }
    virtual ~YMGCPEngine();
    virtual bool processEvent(MGCPTransaction* trans, MGCPMessage* msg, void* data);
private:
    bool createConn(MGCPTransaction* trans, MGCPMessage* msg);
};

class MGCPChan : public Channel
{
    YCLASS(MGCPChan,Channel);
public:
    enum IdType {
	CallId,
	ConnId,
	NtfyId,
    };
    MGCPChan(const char* connId = 0);
    virtual ~MGCPChan();
    virtual void callAccept(Message& msg);
    virtual bool msgTone(Message& msg, const char* tone);
    const String& getId(IdType type) const;
    bool processEvent(MGCPTransaction* tr, MGCPMessage* mm);
    bool initialEvent(MGCPTransaction* tr, MGCPMessage* mm, const MGCPEndpointId& id);
    void activate(bool standby);
protected:
    void disconnected(bool final, const char* reason);
private:
    void endTransaction(int code = 407, const NamedList* params = 0);
    bool reqNotify(String& evt);
    bool setSignal(String& req);
    static void copyRtpParams(NamedList& dest, const NamedList& src);
    MGCPTransaction* m_tr;
    String m_connEp;
    String m_callId;
    String m_ntfyId;
    String m_rtpId;
    bool m_standby;
    bool m_isRtp;
};

class MGCPPlugin : public Driver
{
public:
    MGCPPlugin();
    virtual ~MGCPPlugin();
    virtual bool msgExecute(Message& msg, String& dest);
    virtual void initialize();
    RefPointer<MGCPChan> findConn(const String* id, MGCPChan::IdType type);
    inline RefPointer<MGCPChan> findConn(const String& id, MGCPChan::IdType type)
	{ return findConn(&id,type); }
    void activate(bool standby);
};

class DummyCall : public CallEndpoint
{
public:
    inline DummyCall()
	: CallEndpoint("dummy")
	{ }
};

static MGCPPlugin splugin;

static YMGCPEngine* s_engine = 0;

// warm standby mode
static bool s_standby = false;

// start time as UNIX time
String s_started;

// copy parameter (if present) with new name
bool copyRename(NamedList& dest, const char* dname, const NamedList& src, const String& sname)
{
    if (!sname)
	return false;
    const NamedString* value = src.getParam(sname);
    if (!value)
	return false;
    dest.addParam(dname,*value);
    return true;
}


YMGCPEngine::~YMGCPEngine()
{
    s_engine = 0;
}

// process all MGCP events, distribute them according to their type
bool YMGCPEngine::processEvent(MGCPTransaction* trans, MGCPMessage* msg, void* data)
{
    RefPointer<MGCPChan> chan = YOBJECT(MGCPChan,static_cast<GenObject*>(data));
    Debug(this,DebugAll,"YMGCPEngine::processEvent(%p,%p,%p) [%p]",
	trans,msg,data,this);
    if (!trans)
	return false;
    if (chan)
	return chan->processEvent(trans,msg);
    if (!msg)
	return false;
    if (!data && !trans->outgoing() && msg->isCommand()) {
	if (msg->name() == "CRCX") {
	    // create connection
	    if (!createConn(trans,msg))
		trans->setResponse(500); // unknown endpoint
	    return true;
	}
	if ((msg->name() == "DLCX") || // delete
	    (msg->name() == "MDCX") || // modify
	    (msg->name() == "AUCX")) { // audit
	    // connection must exist already
	    chan = splugin.findConn(msg->params.getParam("i"),MGCPChan::ConnId);
	    if (chan)
		return chan->processEvent(trans,msg);
	    trans->setResponse(515); // no connection
	    return true;
	}
	if (msg->name() == "RQNT") {
	    // request notify
	    chan = splugin.findConn(msg->params.getParam("x"),MGCPChan::NtfyId);
	    if (chan)
		return chan->processEvent(trans,msg);
	}
	if (msg->name() == "EPCF") {
	    // endpoint configuration
	    NamedList params("");
	    bool standby = msg->params.getBoolValue("x-standby",s_standby);
	    if (standby != s_standby) {
		params << "Switching to " << (standby ? "standby" : "active") << " mode";
		Debug(this,DebugNote,"%s",params.c_str());
		s_standby = standby;
		splugin.activate(standby);
	    }
	    params.addParam("x-standby",String::boolText(s_standby));
	    trans->setResponse(200,&params);
	    return true;
	}
	if (msg->name() == "AUEP") {
	    // audit endpoint
	    NamedList params("");
	    params.addParam("MD",String(s_engine->maxRecvPacket()));
	    params.addParam("x-standby",String::boolText(s_standby));
	    params.addParam("x-started",s_started);
	    trans->setResponse(200,&params);
	    return true;
	}
	Debug(this,DebugMild,"Unhandled '%s' from '%s'",
	    msg->name().c_str(),msg->endpointId().c_str());
    }
    return false;
}

// create a new connection
bool YMGCPEngine::createConn(MGCPTransaction* trans, MGCPMessage* msg)
{
    String id = msg->endpointId();
    const char* connId = msg->params.getValue("i");
    DDebug(this,DebugInfo,"YMGCPEngine::createConn() id='%s' connId='%s'",id.c_str(),connId);
    if (connId && splugin.findConn(connId,MGCPChan::ConnId)) {
	trans->setResponse(539,"Connection exists");
	return true;
    }
    MGCPChan* chan = new MGCPChan(connId);
    return chan->initialEvent(trans,msg,id);
}


MGCPChan::MGCPChan(const char* connId)
    : Channel(splugin),
      m_tr(0), m_standby(s_standby), m_isRtp(false)
{
    DDebug(this,DebugAll,"MGCPChan::MGCPChan('%s') [%p]",connId,this);
    status("created");
    if (connId) {
	if (!m_standby)
	    Debug(this,DebugMild,"Using provided connection ID in active mode! [%p]",this);
	m_address = connId;
    }
    else {
	if (m_standby)
	    Debug(this,DebugMild,"Allocating connection ID in standby mode! [%p]",this);
	long int r = ::random();
	m_address.hexify(&r,sizeof(r),0,true);
    }
}

MGCPChan::~MGCPChan()
{
    DDebug(this,DebugAll,"MGCPChan::~MGCPChan() [%p]",this);
    endTransaction();
}

void MGCPChan::disconnected(bool final, const char* reason)
{
    if (final || Engine::exiting())
	return;
    DummyCall* dummy = new DummyCall;
    connect(dummy);
    dummy->deref();
}

const String& MGCPChan::getId(IdType type) const
{
    switch (type) {
	case CallId:
	    return m_callId;
	case ConnId:
	    return address();
	case NtfyId:
	    return m_ntfyId;
	default:
	    return String::empty();
    }
}

void MGCPChan::activate(bool standby)
{
    if (standby == m_standby)
	return;
    Debug(this,DebugCall,"Switching to %s mode. [%p]",standby ? "standby" : "active",this);
    m_standby = standby;
}

void MGCPChan::endTransaction(int code, const NamedList* params)
{
    MGCPTransaction* tr = m_tr;
    m_tr = 0;
    if (!tr)
	return;
    Debug(this,DebugInfo,"Finishing transaction %p with code %d [%p]",tr,code,this);
    tr->userData(0);
    tr->setResponse(code,params);
}

// method called for each event requesting notification
bool MGCPChan::reqNotify(String& evt)
{
    Debug(this,DebugStub,"MGCPChan::reqNotify('%s') [%p]",evt.c_str(),this);
    return false;
}

// method called for each signal request
bool MGCPChan::setSignal(String& req)
{
    Debug(this,DebugStub,"MGCPChan::setSignal('%s') [%p]",req.c_str(),this);
    return false;
}

void MGCPChan::callAccept(Message& msg)
{
    NamedList params("");
    params.addParam("I",address());
    params.addParam("x-standby",String::boolText(m_standby));
    endTransaction(200,&params);
}

bool MGCPChan::msgTone(Message& msg, const char* tone)
{
    if (null(tone))
	return false;
    MGCPEndpoint* ep = s_engine->findEp(m_connEp);
    if (!ep)
	return false;
    MGCPEpInfo* epi = ep->peer();
    if (!epi)
	return false;
    MGCPMessage* mm = new MGCPMessage(s_engine,"NTFY",epi->toString());
    String tmp;
    while (char c = *tone++) {
	if (tmp)
	    tmp << ",";
	tmp << "D/" << c;
    }
    mm->params.setParam("O",tmp);
    return s_engine->sendCommand(mm,epi->address) != 0;
}

bool MGCPChan::processEvent(MGCPTransaction* tr, MGCPMessage* mm)
{
    Debug(this,DebugInfo,"MGCPChan::processEvent(%p,%p) [%p]",tr,mm,this);
    if (!mm) {
	if (m_tr == tr) {
	    Debug(this,DebugInfo,"Clearing transaction %p [%p]",tr,this);
	    m_tr = 0;
	    tr->userData(0);
	}
	return true;
    }
    if (!(m_tr || tr->userData())) {
	Debug(this,DebugInfo,"Acquiring transaction %p [%p]",tr,this);
	m_tr = tr;
	tr->userData(static_cast<GenObject*>(this));
    }
    NamedList params("");
    params.addParam("I",address());
    params.addParam("x-standby",String::boolText(m_standby));
    if (mm->name() == "DLCX") {
	disconnect();
	status("deleted");
	clearEndpoint();
	m_address.clear();
	tr->setResponse(250,&params);
	return true;
    }
    if (mm->name() == "MDCX") {
	NamedString* param = mm->params.getParam("z2");
	if (param) {
	    // native connect requested
	    RefPointer<MGCPChan> chan2 = splugin.findConn(*param,MGCPChan::ConnId);
	    if (!chan2) {
		tr->setResponse(515); // no connection
		return true;
	    }
	    if (!connect(chan2,mm->params.getValue("x-reason","bridged"))) {
		tr->setResponse(400); // unspecified error
		return true;
	    }
	}
	param = mm->params.getParam("x");
	if (param)
	    m_ntfyId = *param;
	if (m_isRtp) {
	    Message m("chan.rtp");
	    m.addParam("mgcp_allowed",String::boolText(false));
	    copyRtpParams(m,mm->params);
	    if (m_rtpId)
		m.setParam("rtpid",m_rtpId);
	    m.userData(this);
	    if (Engine::dispatch(m)) {
		copyRename(params,"x-localip",m,"localip");
		copyRename(params,"x-localport",m,"localport");
		m_rtpId = m.getValue("rtpid",m_rtpId);
	    }
	}
	tr->setResponse(200,&params);
	return true;
    }
    if (mm->name() == "AUCX") {
	tr->setResponse(200,&params);
	return true;
    }
    if (mm->name() == "RQNT") {
	bool ok = true;
	// what we are requested to notify back
	NamedString* req = mm->params.getParam("r");
	if (req) {
	    ObjList* lst = req->split(',');
	    for (ObjList* item = lst->skipNull(); item; item = item->skipNext())
		ok = reqNotify(*static_cast<String*>(item->get())) && ok;
	    delete lst;
	}
	// what we must signal now
	req = mm->params.getParam("s");
	if (req) {
	    ObjList* lst = req->split(',');
	    for (ObjList* item = lst->skipNull(); item; item = item->skipNext())
		ok = setSignal(*static_cast<String*>(item->get())) && ok;
	    delete lst;
	}
	tr->setResponse(ok ? 200 : 538,&params);
	return true;
    }
    return false;
}

bool MGCPChan::initialEvent(MGCPTransaction* tr, MGCPMessage* mm, const MGCPEndpointId& id)
{
    Debug(this,DebugInfo,"MGCPChan::initialEvent(%p,%p,'%s') [%p]",
	tr,mm,id.id().c_str(),this);
    m_connEp = id.id();
    m_callId = mm->params.getValue("c");
    m_ntfyId = mm->params.getValue("x");

    if (id.user() == "gigi")
	m_isRtp = true;

    Message* m = message(m_isRtp ? "chan.rtp" : "call.route");
    m->addParam("mgcp_allowed",String::boolText(false));
    copyRtpParams(*m,mm->params);
    if (m_isRtp) {
	m->userData(this);
	bool ok = Engine::dispatch(m);
	if (!ok) {
	    delete m;
	    deref();
	    return false;
	}
	NamedList params("");
	params.addParam("I",address());
	params.addParam("x-standby",String::boolText(m_standby));
	copyRename(params,"x-localip",*m,"localip");
	copyRename(params,"x-localport",*m,"localport");
	m_rtpId = m->getValue("rtpid");
	delete m;
	tr->setResponse(200,&params);
	DummyCall* dummy = new DummyCall;
	connect(dummy);
	dummy->deref();
	deref();
	return true;
    }
    m_tr = tr;
    tr->userData(static_cast<GenObject*>(this));
    m->addParam("called",id.id());
    if (startRouter(m)) {
	tr->sendProvisional();
	return true;
    }
    return false;
}

void MGCPChan::copyRtpParams(NamedList& dest, const NamedList& src)
{
    copyRename(dest,"transport",src,"x-transport");
    copyRename(dest,"media",src,"x-media");
    copyRename(dest,"localip",src,"x-localip");
    copyRename(dest,"localport",src,"x-localport");
    copyRename(dest,"remoteip",src,"x-remoteip");
    copyRename(dest,"remoteport",src,"x-remoteport");
    copyRename(dest,"payload",src,"x-payload");
    copyRename(dest,"evpayload",src,"x-evpayload");
    copyRename(dest,"format",src,"x-format");
    copyRename(dest,"direction",src,"x-direction");
    copyRename(dest,"ssrc",src,"x-ssrc");
    copyRename(dest,"drillhole",src,"x-drillhole");
    copyRename(dest,"autoaddr",src,"x-autoaddr");
    copyRename(dest,"anyssrc",src,"x-anyssrc");
}

MGCPPlugin::MGCPPlugin()
    : Driver("mgcpgw","misc")
{
    Output("Loaded module MGCP-GW");
}

MGCPPlugin::~MGCPPlugin()
{
    Output("Unloading module MGCP-GW");
    delete s_engine;
}

bool MGCPPlugin::msgExecute(Message& msg, String& dest)
{
    Debug(this,DebugWarn,"Received execute request for gateway '%s'",dest.c_str());
    return false;
}

RefPointer<MGCPChan> MGCPPlugin::findConn(const String* id, MGCPChan::IdType type)
{
    if (!id || id->null())
	return 0;
    Lock lock(this);
    for (ObjList* l = channels().skipNull(); l; l = l->skipNext()) {
	MGCPChan* c = static_cast<MGCPChan*>(l->get());
	if (c->getId(type) == *id)
	    return c;
    }
    return 0;
}

void MGCPPlugin::activate(bool standby)
{
    lock();
    ListIterator iter(channels());
    while (GenObject* obj = iter.get()) {
	RefPointer<MGCPChan> chan = static_cast<MGCPChan*>(obj);
	if (chan) {
	    unlock();
	    chan->activate(standby);
	    lock();
	}
    }
    unlock();
}

void MGCPPlugin::initialize()
{
    Output("Initializing module MGCP Gateway");
    Configuration cfg(Engine::configFile("mgcpgw"));
    setup();
    NamedList* sect = cfg.getSection("engine");
    if (s_engine && sect)
	s_engine->initialize(*sect);
    while (!s_engine) {
	if (!(sect && sect->getBoolValue("enabled",true)))
	    break;
	s_started = Time::secNow();
	s_standby = cfg.getBoolValue("general","standby",false);
	s_engine = new YMGCPEngine(sect);
	s_engine->debugChain(this);
	int n = cfg.sections();
	for (int i = 0; i < n; i++) {
	    sect = cfg.getSection(i);
	    if (!sect)
		continue;
	    String name(*sect);
	    if (name.startSkip("ep") && name) {
		MGCPEndpoint* ep = new MGCPEndpoint(
		    s_engine,
		    sect->getValue("local_user",name),
		    sect->getValue("local_host",s_engine->address().host()),
		    sect->getIntValue("local_port")
		);
		MGCPEpInfo* ca = ep->append(0,
		    sect->getValue("remote_host"),
		    sect->getIntValue("remote_port",0)
		);
		if (ca) {
		    if (sect->getBoolValue("announce",true)) {
			MGCPMessage* mm = new MGCPMessage(s_engine,"RSIP",ep->toString());
			mm->params.addParam("RM","restart");
			mm->params.addParam("x-standby",String::boolText(s_standby));
			mm->params.addParam("x-started",s_started);
			s_engine->sendCommand(mm,ca->address);
		    }
		}
		else
		    Debug(this,DebugWarn,"Could not set remote endpoint for '%s'",
			name.c_str());
	    }
	}
    }
}

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */
