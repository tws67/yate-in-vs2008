/**
 * enumroute.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * ENUM routing module
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

#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>


using namespace TelEngine;
namespace { // anonymous

#define ENUM_DEF_TIMEOUT 3
#define ENUM_DEF_RETRIES 2
#define ENUM_DEF_MINLEN  8
#define ENUM_DEF_MAXCALL 30000

class NAPTR : public GenObject
{
public:
    NAPTR(int ord, int pref, const char* flags, const char* serv, const char* regexp, const char* replace);
    bool replace(String& str);
    inline int order() const
	{ return m_order; }
    inline int pref() const
	{ return m_pref; }
    inline const String& flags() const
	{ return m_flags; }
    inline const String& serv() const
	{ return m_service; }

private:
    int m_order;
    int m_pref;
    String m_flags;
    String m_service;
    Regexp m_regmatch;
    String m_template;
    String m_replace;
};

class EnumHandler : public MessageHandler
{
public:
    inline EnumHandler(unsigned int prio = 90)
	: MessageHandler("call.route",prio)
	{ }
    virtual bool received(Message& msg);
private:
    static bool resolve(Message& msg,bool canRedirect);
    static void addRoute(String& dest,const String& src);
};

class EnumModule : public Module
{
public:
    inline EnumModule()
	: Module("enumroute","route"), m_init(false)
	{ }
    virtual void initialize();
    virtual void statusParams(String& str);
    void genUpdate(Message& msg);
private:
    bool m_init;
};

// weird but NS_MAXSTRING and dn_string() are NOT part of the resolver API...
#ifndef NS_MAXSTRING
#define NS_MAXSTRING 255
#endif

// copy one string (not domain) from response
static int dn_string(const unsigned char* end, const unsigned char* src, char *dest, int maxlen)
{
    int n = src[0];
    maxlen--;
    if (maxlen > n)
	maxlen = n;
    if (dest && (maxlen > 0)) {
	while ((maxlen-- > 0) && (src < end))
	    *dest++ = *++src;
	*dest = 0;
    }
    return n+1;
}


static String s_prefix;
static String s_forkStop;
static String s_domains;
static unsigned int s_minlen;
static int s_timeout;
static int s_retries;
static int s_maxcall;

static bool s_redirect;
static bool s_autoFork;
static bool s_sipUsed;
static bool s_iaxUsed;
static bool s_h323Used;
static bool s_xmppUsed;
static bool s_telUsed;
static bool s_voiceUsed;
static bool s_pstnUsed;
static bool s_voidUsed;

static Mutex s_mutex;
static int s_queries = 0;
static int s_routed = 0;
static int s_reroute = 0;

static EnumModule emodule;

// Initializes the resolver library in the current thread
static bool resolvInit()
{
    if ((_res.options & RES_INIT) == 0) {
	// need to initialize in this thread
	if (res_init())
	    return false;
    }
    // always set the timeout variables
    _res.retrans = s_timeout;
    _res.retry = s_retries;
    return true;
}

// Perform DNS query, return list of only NAPTR records
static ObjList* naptrQuery(const char* dname)
{
    unsigned char buf[2048];
    int r,q,a;
    unsigned char *p, *e;
    DDebug(&emodule,DebugInfo,"Querying %s",dname);
    r = res_query(dname,ns_c_in,ns_t_naptr,
	buf,sizeof(buf));
    XDebug(&emodule,DebugAll,"res_query %d",r);
    if ((r < 0) || (r > (int)sizeof(buf)))
	return 0;
    p = buf+NS_QFIXEDSZ;
    NS_GET16(q,p);
    NS_GET16(a,p);
    XDebug(&emodule,DebugAll,"questions: %d, answers: %d",q,a);
    p = buf + NS_HFIXEDSZ;
    e = buf + r;
    for (; q > 0; q--) {
	int n = dn_skipname(p,e);
	if (n < 0)
	    return 0;
	p += (n + NS_QFIXEDSZ);
    }
    XDebug(&emodule,DebugAll,"skipped questions");
    ObjList* lst = 0;
    for (; a > 0; a--) {
	int ty,cl,sz;
	long int tt;
	char name[NS_MAXLABEL+1];
	unsigned char* l;
	int n = dn_expand(buf,e,p,name,sizeof(name));
	if ((n <= 0) || (n > NS_MAXLABEL))
	    return lst;
	buf[n] = 0;
	p += n;
	NS_GET16(ty,p);
	NS_GET16(cl,p);
	NS_GET32(tt,p);
	NS_GET16(sz,p);
	XDebug(&emodule,DebugAll,"found '%s' type %d size %d",name,ty,sz);
	l = p;
	p += sz;
	if (ty == ns_t_naptr) {
	    int ord,pr;
	    char fla[NS_MAXSTRING+1];
	    char ser[NS_MAXSTRING+1];
	    char reg[NS_MAXSTRING+1];
	    char rep[NS_MAXLABEL+1];
	    NS_GET16(ord,l);
	    NS_GET16(pr,l);
	    n = dn_string(e,l,fla,sizeof(fla));
	    l += n;
	    n = dn_string(e,l,ser,sizeof(ser));
	    l += n;
	    n = dn_string(e,l,reg,sizeof(reg));
	    l += n;
	    n = dn_expand(buf,e,l,rep,sizeof(rep));
	    l += n;
	    DDebug(&emodule,DebugAll,"order=%d pref=%d flags='%s' serv='%s' regexp='%s' replace='%s'",
		ord,pr,fla,ser,reg,rep);
	    if (!lst)
		lst = new ObjList;
	    NAPTR* ptr;
	    ObjList* cur = lst;
	    // cycle existing records, insert at the right place
	    for (; cur; cur = cur->next()) {
		ptr = static_cast<NAPTR*>(cur->get());
		if (!ptr)
		    continue;
		if (ptr->order() > ord)
		    break;
		if (ptr->order() < ord)
		    continue;
		// sort first by order and then by preference
		if (ptr->pref() > pr)
		    break;
	    }
	    ptr = new NAPTR(ord,pr,fla,ser,reg,rep);
	    if (cur)
		cur->insert(ptr);
	    else
		lst->append(ptr);
	}
    }
    return lst;
}


NAPTR::NAPTR(int ord, int pref, const char* flags, const char* serv, const char* regexp, const char* replace)
    : m_order(ord), m_pref(pref), m_flags(flags), m_service(serv), m_replace(replace)
{
    // use case-sensitive extended regular expressions
    m_regmatch.setFlags(true,false);
    if (!null(regexp)) {
	// look for <sep>regexp<sep>template<sep>
	char sep[2] = { regexp[0], 0 };
	String tmp(regexp+1);
	if (tmp.endsWith(sep)) {
	    int pos = tmp.find(sep);
	    if (pos > 0) {
		m_regmatch = tmp.substr(0,pos);
		m_template = tmp.substr(pos+1,tmp.length()-pos-2);
		XDebug(&emodule,DebugAll,"NAPTR match '%s' template '%s'",m_regmatch.c_str(),m_template.c_str());
	    }
	}
    }
}

// Perform the Regexp replacement, return true if succeeded
bool NAPTR::replace(String& str)
{
    if (m_regmatch && str.matches(m_regmatch)) {
	str = str.replaceMatches(m_template);
	return true;
    }
    return false;
}


// Routing message handler, performs checks and calls resolve method
bool EnumHandler::received(Message& msg)
{
    if (s_domains.null() || !msg.getBoolValue("enumroute",true))
	return false;
    // perform per-thread initialization of resolver and timeout settings
    if (!resolvInit())
	return false;
    return resolve(msg,s_telUsed);
}

// Resolver function, may call itself recursively at most once
bool EnumHandler::resolve(Message& msg,bool canRedirect)
{
    // give preference to full (e164) called number if exists
    String called(msg.getValue("calledfull"));
    if (called.null())
	called = msg.getValue("called");
    if (called.null())
	return false;
    // check if the called starts with international prefix, remove it
    if (!(called.startSkip("+",false) ||
	 (s_prefix && called.startSkip(s_prefix,false))))
	return false;
    if (called.length() < s_minlen)
	return false;
    s_mutex.lock();
    ObjList* domains = s_domains.split(',',false);
    s_mutex.unlock();
    if (!domains)
	return false;
    bool rval = false;
    // put the standard international prefix in front
    called = "+" + called;
    String tmp;
    for (int i = called.length()-1; i > 0; i--)
	tmp << called.at(i) << ".";
    u_int64_t dt = Time::now();
    ObjList* res = 0;
    for (ObjList* l = domains; l; l = l->next()) {
	const String* s = static_cast<const String*>(l->get());
	if (!s || s->null())
	    continue;
	res = naptrQuery(tmp + *s);
	if (res)
	    break;
    }
    dt = Time::now() - dt;
    Debug(&emodule,DebugInfo,"Returned %d NAPTR records in %u.%06u s",
	res ? res->count() : 0,
	(unsigned int)(dt / 1000000),
	(unsigned int)(dt % 1000000));
    domains->destruct();
    bool reroute = false;
    bool unassigned = false;
    if (res) {
	msg.retValue().clear();
	bool autoFork = msg.getBoolValue("autofork",s_autoFork);
	ObjList* cur = res;
	for (; cur; cur = cur->next()) {
	    NAPTR* ptr = static_cast<NAPTR*>(cur->get());
	    if (!ptr)
		continue;
	    DDebug(&emodule,DebugAll,"order=%d pref=%d '%s'",
		ptr->order(),ptr->pref(),ptr->serv().c_str());
	    String serv = ptr->serv();
	    serv.toUpper();
	    String callto = called;
	    if (s_sipUsed && (serv == "E2U+SIP") && ptr->replace(callto)) {
		addRoute(msg.retValue(),"sip/" + callto);
		rval = true;
		if (autoFork)
		    continue;
		break;
	    }
	    if (s_iaxUsed && (serv == "E2U+IAX2") && ptr->replace(callto)) {
		addRoute(msg.retValue(),"iax/" + callto);
		rval = true;
		if (autoFork)
		    continue;
		break;
	    }
	    if (s_h323Used && (serv == "E2U+H323") && ptr->replace(callto)) {
		addRoute(msg.retValue(),"h323/" + callto);
		rval = true;
		if (autoFork)
		    continue;
		break;
	    }
	    if (s_xmppUsed && (serv == "E2U+XMPP") && ptr->replace(callto)) {
		addRoute(msg.retValue(),"jingle/" + callto);
		rval = true;
		if (autoFork)
		    continue;
		break;
	    }
	    if (s_pstnUsed && serv.startsWith("E2U+PSTN") && ptr->replace(callto)) {
		addRoute(msg.retValue(),"pstn/" + callto);
		rval = true;
		if (autoFork)
		    continue;
		break;
	    }
	    if (s_voiceUsed && serv.startsWith("E2U+VOICE") && ptr->replace(callto)) {
		addRoute(msg.retValue(),"voice/" + callto);
		rval = true;
		if (autoFork)
		    continue;
		break;
	    }
	    if (canRedirect && (serv == "E2U+TEL") && ptr->replace(callto)) {
		if (callto.startSkip("tel:",false) ||
		    callto.startSkip("TEL:",false) ||
		    callto.startSkip("e164:",false) ||
		    callto.startSkip("E164:",false))
		{
		    reroute = true;
		    rval = false;
		    msg.setParam("called",callto);
		    msg.clearParam("calledfull");
		    if (msg.retValue()) {
			Debug(&emodule,DebugMild,"Redirect drops collected route: %s",
			    msg.retValue().c_str());
			msg.retValue().clear();
		    }
		    break;
		}
		continue;
	    }
	    if (s_voidUsed && serv.startsWith("E2U+VOID") && ptr->replace(callto)) {
		// remember it's unassigned but still continue scanning
		unassigned = true;
	    }
	}
	res->destruct();
    }
    s_mutex.lock();
    if (rval) {
	if (msg.retValue().startsWith("fork",true)) {
	    msg.setParam("maxcall",String(s_maxcall));
	    msg.setParam("fork.stop",s_forkStop);
	}
        else if (s_redirect)
	    msg.setParam("redirect",String::boolText(true));
    }
    s_queries++;
    if (rval)
	s_routed++;
    if (reroute)
	s_reroute++;
    emodule.changed();
    s_mutex.unlock();
    if (reroute)
	return resolve(msg,false);
    if (unassigned && !rval) {
	rval = true;
	msg.retValue() = "-";
	msg.setParam("error","unallocated");
    }
    return rval;
}

// Add one route to the result, take care of forking
void EnumHandler::addRoute(String& dest,const String& src)
{
    if (dest.null())
	dest = src;
    else {
	if (!dest.startsWith("fork",true))
	    dest = "fork " + dest;
	dest << " | " << src;
    }
}


void EnumModule::statusParams(String& str)
{
    str.append("queries=",",") << s_queries << ",routed=" << s_routed << ",rerouted=" << s_reroute;
}

void EnumModule::genUpdate(Message& msg)
{
    msg.setParam("queries",String(s_queries));
    msg.setParam("routed",String(s_routed));
    msg.setParam("rerouted",String(s_reroute));
}

void EnumModule::initialize()
{
    Module::initialize();
    Configuration cfg(Engine::configFile("enumroute"));
    int prio = cfg.getIntValue("general","priority",0);
    if ((prio <= 0) && !m_init)
	return;
    Output("Initializing ENUM routing");
    s_mutex.lock();
    // in most of the world this default international prefix should work
    s_prefix = cfg.getValue("general","prefix","00");
    s_domains = cfg.getValue("general","domains");
    if (s_domains.null()) {
	// old style, just for compatibility
	s_domains = cfg.getValue("general","domain","e164.arpa");
	s_domains.append(cfg.getValue("general","backup","e164.org"),",");
    }
    s_forkStop = cfg.getValue("general","forkstop","busy");
    s_mutex.unlock();
    DDebug(&emodule,DebugInfo,"Domain list: %s",s_domains.c_str());
    s_minlen = cfg.getIntValue("general","minlen",ENUM_DEF_MINLEN);
    int tmp = cfg.getIntValue("general","timeout",ENUM_DEF_TIMEOUT);
    // limit between 1 and 10 seconds
    if (tmp < 1)
	tmp = 1;
    if (tmp > 10)
	tmp = 10;
    s_timeout = tmp;
    tmp = cfg.getIntValue("general","retries",ENUM_DEF_RETRIES);
    // limit between 1 and 5 retries
    if (tmp < 1)
	tmp = 1;
    if (tmp > 5)
	tmp = 5;
    s_retries = tmp;
    // overall a resolve attempt will take at most 50s per domain

    tmp = cfg.getIntValue("general","maxcall",ENUM_DEF_MAXCALL);
    // limit between 2 and 120 seconds
    if (tmp < 2000)
	tmp = 2000;
    if (tmp > 120000)
	tmp = 120000;
    s_maxcall = tmp;

    s_redirect = cfg.getBoolValue("general","redirect");
    s_autoFork = cfg.getBoolValue("general","autofork");
    s_sipUsed  = cfg.getBoolValue("protocols","sip",true);
    s_iaxUsed  = cfg.getBoolValue("protocols","iax",true);
    s_h323Used = cfg.getBoolValue("protocols","h323",true);
    s_xmppUsed = cfg.getBoolValue("protocols","jingle",true);
    s_voidUsed = cfg.getBoolValue("protocols","void",true);
    // by default don't support the number rerouting
    s_telUsed  = cfg.getBoolValue("protocols","tel",false);
    // also don't enable gateways by default as more setup is needed
    s_pstnUsed = cfg.getBoolValue("protocols","pstn",false);
    s_voiceUsed= cfg.getBoolValue("protocols","voice",false);
    if (m_init || (prio <= 0))
	return;
    m_init = true;
    int res = res_init();
    if (res)
	Debug(&emodule,DebugGoOn,"res_init returned error %d",res);
    else
	Engine::install(new EnumHandler(cfg.getIntValue("general","priority",prio)));
}

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */
