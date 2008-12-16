/**
 * clustering.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Clustering server support.
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

using namespace TelEngine;
namespace { // anonymous

class ClusterModule : public Module
{
public:
    enum {
	Register = Private,
    };
    ClusterModule();
    ~ClusterModule();
    bool unload();
    virtual void initialize();
    virtual bool received(Message& msg, int id);
    virtual bool msgRoute(Message& msg);
    virtual bool msgExecute(Message& msg);
    virtual bool msgRegister(Message& msg);
private:
    String m_prefix;
    String m_callto;
    Regexp m_regexp;
    String m_message;
    bool m_init;
};

INIT_PLUGIN(ClusterModule);

UNLOAD_PLUGIN(unloadNow)
{
    if (unloadNow && !__plugin.unload())
	return false;
    return true;
}


bool ClusterModule::unload()
{
    if (!lock(500000))
	return false;
    uninstallRelays();
    unlock();
    return true;
}

bool ClusterModule::msgRoute(Message& msg)
{
    String called = msg.getValue("called");
    if (called.null())
	return false;
    Lock lock(this);
    if (!called.startSkip(m_prefix,false))
	return false;
    lock.drop();
    if (called.trimBlanks().null())
	return false;
    Debug(&__plugin,DebugInfo,"Got call to '%s' on this node",called.c_str());
    msg.setParam("called",called);
    return false;
}

bool ClusterModule::msgExecute(Message& msg)
{
    String callto = msg.getValue("callto");
    if (callto.null())
	return false;
    String tmp = callto;
    Lock lock(this);
    if (!callto.startSkip(m_prefix,false))
	return false;
    int sep = callto.find("/");
    if (sep < 0)
	return false;
    String node = callto.substr(0,sep).trimBlanks();
    callto = callto.substr(sep+1);
    if (callto.trimBlanks().null())
	return false;
    DDebug(&__plugin,DebugAll,"Call to '%s' on node '%s'",callto.c_str(),node.c_str());
    // check if the node is to be dynamically allocated
    if ((node == "*") && m_message) {
	Message m(m_message);
	m.addParam("allocate",String::boolText(true));
	m.addParam("nodename",Engine::nodeName());
	m.addParam("callto",callto);
	const char* param = msg.getValue("billid");
	if (param)
	    m.addParam("billid",param);
	param = msg.getValue("username");
	    m.addParam("username",param);
	if (!Engine::dispatch(m) || (m.retValue() == "-") || (m.retValue() == "error")) {
	    const char* error = m.getValue("error","failure");
	    const char* reason = m.getValue("reason");
	    Debug(&__plugin,DebugWarn,"Could not get node for '%s'%s%s%s%s",
		callto.c_str(),
		(error ? ": " : ""), c_safe(error),
		(reason ? ": " : ""), c_safe(reason));
	    if (error)
		msg.setParam("error",error);
	    else
		msg.clearParam("error");
	    if (reason)
		msg.setParam("reason",reason);
	    else
		msg.clearParam("reason");
	    return false;
	}
	node = m.retValue();
	Debug(&__plugin,DebugInfo,"Using node '%s' for '%s'",
	    node.c_str(),callto.c_str());
    }
    msg.setParam("callto",callto);
    // if the call is for the local node just let it through
    if (node.null() || (Engine::nodeName() == node))
	return false;
    if (!node.matches(m_regexp)) {
	msg.setParam("callto",tmp);
	return false;
    }
    String dest = node.replaceMatches(m_callto);
    lock.drop();
    msg.replaceParams(dest);
    if (dest.trimBlanks().null()) {
	msg.setParam("callto",tmp);
	return false;
    }
    Debug(&__plugin,DebugNote,"Call to '%s' on node '%s' goes to '%s'",
	callto.c_str(),node.c_str(),dest.c_str());
    msg.setParam("callto",dest);
    msg.setParam("osip_x-callto",callto);
    msg.setParam("osip_x-billid",msg.getValue("billid"));
    msg.setParam("osip_x-nodename",Engine::nodeName());
    msg.setParam("osip_x-username",msg.getValue("username"));
    return false;
}

bool ClusterModule::msgRegister(Message& msg)
{
    String data = msg.getValue("data");
    if (data.null())
	return false;
    Lock lock(this);
    if (data.startsWith(m_prefix))
	return false;
    msg.setParam("data",m_prefix + Engine::nodeName() + "/" + data);
    return false;
}

bool ClusterModule::received(Message& msg, int id)
{
    switch (id) {
	case Execute:
	    return msgExecute(msg);
	case Register:
	    return msgRegister(msg);
	default:
	    return Module::received(msg,id);
    }
}

ClusterModule::ClusterModule()
    : Module("clustering","misc",true),
      m_init(false)
{
    Output("Loaded module Clustering");
}

ClusterModule::~ClusterModule()
{
    Output("Unloading module Clustering");
}

void ClusterModule::initialize()
{
    if (Engine::nodeName().null()) {
	Debug(&__plugin,DebugNote,"Node name is empty, clustering disabled.");
	return;
    }
    Output("Initializing module Clustering");
    Configuration cfg(Engine::configFile("clustering"));
    lock();
    m_prefix = cfg.getValue("general","prefix","cluster");
    if (!m_prefix.endsWith("/"))
	m_prefix += "/";
    m_regexp = cfg.getValue("general","regexp");
    m_callto = cfg.getValue("general","callto");
    m_message = cfg.getValue("general","locate","cluster.locate");
    unlock();
    if (!m_init && cfg.getBoolValue("general","enabled",(m_callto && m_regexp))) {
	setup();
	installRelay(Route,cfg.getIntValue("priorities","call.route",50));
	installRelay(Execute,cfg.getIntValue("priorities","call.execute",50));
	installRelay(Register,"user.register",cfg.getIntValue("priorities","user.register",50));
	m_init = true;
    }
}

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */
