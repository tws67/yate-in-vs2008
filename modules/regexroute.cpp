/**
 * regexroute.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Regular expressions based routing
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

#include <stdlib.h>
#include <string.h>

using namespace TelEngine;
namespace { // anonymous

static Configuration s_cfg;
static bool s_extended;
static bool s_insensitive;
static Mutex s_mutex;
static ObjList s_extra;
static NamedList s_vars("");

class RouteHandler : public MessageHandler
{
public:
    RouteHandler(int prio)
	: MessageHandler("call.route",prio) { }
    virtual bool received(Message &msg);
};

static String& vars(String& s, String* vName = 0)
{
    if (s.startSkip("$",false)) {
	s.trimBlanks();
	if (vName)
	    *vName = s;
	s = s_vars.getValue(s);
    }
    return s;
}

enum {
    OPER_ADD,
    OPER_SUB,
    OPER_MUL,
    OPER_DIV,
    OPER_MOD,
    OPER_EQ,
    OPER_NE,
    OPER_GT,
    OPER_LT,
    OPER_GE,
    OPER_LE,
};

static void mathOper(String& str, String& par, int sep, int oper)
{
    str = par.substr(0,sep);
    par = par.substr(sep+1);
    int len = str.length();
    sep = par.find(',');
    if (sep > 0) {
	String tmp = par.substr(sep+1);
	len = vars(tmp).toInteger();
	par = par.substr(0,sep);
    }
    int p1 = vars(str).toInteger(0,10);
    int p2 = vars(par).toInteger(0,10);
    switch (oper) {
	case OPER_ADD:
	    str = p1+p2;
	    break;
	case OPER_SUB:
	    str = p1-p2;
	    break;
	case OPER_MUL:
	    str = p1*p2;
	    break;
	case OPER_DIV:
	    str = p2 ? p1/p2 : 0;
	    break;
	case OPER_MOD:
	    str = p2 ? p1%p2 : 0;
	    break;
	case OPER_EQ:
	    str = (p1 == p2);
	    len = 0;
	    return;
	case OPER_NE:
	    str = (p1 != p2);
	    len = 0;
	    return;
	case OPER_GT:
	    str = (p1 > p2);
	    len = 0;
	    return;
	case OPER_LT:
	    str = (p1 < p2);
	    len = 0;
	    return;
	case OPER_GE:
	    str = (p1 >= p2);
	    len = 0;
	    return;
	case OPER_LE:
	    str = (p1 <= p2);
	    len = 0;
	    return;
    }
    len -= (int)str.length();
    if (len > 0) {
	// left pad the result to the desired length
	String tmp('0',len);
	if (str[0] == '-')
	    str = "-" + tmp + str.substr(1);
	else
	    str = tmp + str;
    }
}

static void evalFunc(String& str)
{
    if (str.null())
	str = ";";
    else if (str.startSkip("++",false)) {
	String tmp;
	str = vars(str,&tmp).toInteger(0,10) + 1;
	if (tmp)
	    s_vars.setParam(tmp,str);
    }
    else if (str.startSkip("--",false)) {
	String tmp;
	str = vars(str,&tmp).toInteger(0,10) - 1;
	if (tmp)
	    s_vars.setParam(tmp,str);
    }
    else {
	int sep = str.find(',');
	String par;
	if (sep > 0) {
	    par = str.substr(sep+1);
	    str = str.substr(0,sep);
	    sep = par.find(',');
	}
	if (str == "length")
	    str = vars(par).length();
	else if (str == "upper")
	    str = vars(par).toUpper();
	else if (str == "lower")
	    str = vars(par).toLower();
	else if ((sep > 0) && ((str == "streq") || (str == "strne"))) {
	    bool ret = (str == "strne");
	    str = par.substr(sep+1);
	    par = par.substr(0,sep);
	    vars(str);
	    vars(par);
	    ret ^= (str == par);
	    str = ret;
	}
	else if ((sep > 0) && ((str == "add") || (str == "+")))
	    mathOper(str,par,sep,OPER_ADD);
	else if ((sep > 0) && ((str == "sub") || (str == "-")))
	    mathOper(str,par,sep,OPER_SUB);
	else if ((sep > 0) && ((str == "mul") || (str == "*")))
	    mathOper(str,par,sep,OPER_MUL);
	else if ((sep > 0) && ((str == "div") || (str == "/")))
	    mathOper(str,par,sep,OPER_DIV);
	else if ((sep > 0) && ((str == "mod") || (str == "%")))
	    mathOper(str,par,sep,OPER_MOD);
	else if ((sep > 0) && (str == "eq"))
	    mathOper(str,par,sep,OPER_EQ);
	else if ((sep > 0) && (str == "ne"))
	    mathOper(str,par,sep,OPER_NE);
	else if ((sep > 0) && ((str == "gt") || (str == ">")))
	    mathOper(str,par,sep,OPER_GT);
	else if ((sep > 0) && ((str == "lt") || (str == "<")))
	    mathOper(str,par,sep,OPER_LT);
	else if ((sep > 0) && (str == "ge"))
	    mathOper(str,par,sep,OPER_GE);
	else if ((sep > 0) && (str == "le"))
	    mathOper(str,par,sep,OPER_LE);
	else if (str == "random") {
	    str.clear();
	    vars(par);
	    for (unsigned int i = 0; i < par.length(); i++) {
		if (par.at(i) == '?')
		    str << (int)(::random() % 10);
		else
		    str << par.at(i);
	    }
	}
	else if ((sep > 0) && ((str == "index") || (str == "rotate"))) {
	    bool rotate = (str == "rotate");
	    String vname;
	    str = par.substr(0,sep);
	    par = par.substr(sep+1).trimBlanks();
	    int idx = vars(str,&vname).toInteger(0,10);
	    ObjList* lst = par.split(',');
	    str.clear();
	    par.clear();
	    unsigned int n = lst->count();
	    if (n) {
		int i = idx % n;
		for (ObjList* l = lst->skipNull(); l; l = l->skipNext()) {
		    String* s = static_cast<String*>(l->get());
		    vars(*s);
		    if (rotate) {
			if (i > 0)
			    par.append(*s," ");
			else
			    str.append(*s," ");
		    }
		    else if (0 == i) {
			str = *s;
			break;
		    }
		    i--;
		}
		str.append(par," ");
		// auto increment the index variable if any
		if (vname) {
		    par = (idx + 1) % n;
		    s_vars.setParam(vname,par);
		}
	    }
	    lst->destruct();
	}
	else if (str == "runid") {
	    str.clear();
	    str << Engine::runId();
	}
	else if (str == "nodename")
	    str = Engine::nodeName();
	else if ((sep >= 0) && (str == "transcode")) {
	    str = par.substr(0,sep);
	    par = par.substr(sep+1).trimBlanks();
	    ObjList* fmts = DataTranslator::allFormats(par,
		(str.find('e') < 0),
		(str.find('r') < 0),
		(str.find('c') < 0));
	    str.clear();
	    str.append(fmts,",");
	    TelEngine::destruct(fmts);
	}
	else if ((sep < 0) && str.trimBlanks())
	    str = s_vars.getValue(str);
	else {
	    Debug("RegexRoute",DebugWarn,"Invalid function '%s'",str.c_str());
	    str.clear();
	}
    }
}

// handle $(function) replacements
static void replaceFuncs(String &str)
{
    int p1;
    while ((p1 = str.find("$(")) >= 0) {
	int p2 = str.find(')',p1+2);
	if (p2 > 0) {
	    String v = str.substr(p1+2,p2-p1-2);
	    v.trimBlanks();
	    DDebug("RegexRoute",DebugAll,"Replacing function '%s'",
		v.c_str());
	    evalFunc(v);
	    str = str.substr(0,p1) + v + str.substr(p2+1);
	}
    }
}

// handle ;paramname[=value] assignments
static void setMessage(Message& msg, String& line, Message* target = 0)
{
    if (!target)
	target = &msg;
    ObjList *strs = line.split(';');
    bool first = true;
    for (ObjList *p = strs; p; p=p->next()) {
	String *s = static_cast<String*>(p->get());
	if (s) {
	    msg.replaceParams(*s);
	    replaceFuncs(*s);
	}
	if (first) {
	    first = false;
	    line = s ? *s : String::empty();
	    continue;
	}
	if (s && !s->trimBlanks().null()) {
	    int q = s->find('=');
	    if (q > 0) {
		String n = s->substr(0,q);
		String v = s->substr(q+1);
		n.trimBlanks();
		v.trimBlanks();
		DDebug("RegexRoute",DebugAll,"Setting '%s' to '%s'",n.c_str(),v.c_str());
		if (n.startSkip("$",false))
		    s_vars.setParam(n,v);
		else
		    target->setParam(n,v);
	    }
	    else {
		DDebug("RegexRoute",DebugAll,"Clearing parameter '%s'",s->c_str());
		if (s->startSkip("$",false))
		    s_vars.clearParam(*s);
		else
		    target->clearParam(*s);
	    }
	}
    }
    strs->destruct();
}

// process one context, can call itself recursively
static bool oneContext(Message &msg, String &str, const String &context, String &ret, int depth = 0)
{
    if (context.null())
	return false;
    if (depth > 5) {
	Debug("RegexRoute",DebugWarn,"Possible loop detected, current context '%s'",context.c_str());
	return false;
    }
    NamedList *l = s_cfg.getSection(context);
    if (l) {
	unsigned int len = l->length();
	for (unsigned int i=0; i<len; i++) {
	    NamedString *n = l->getParam(i);
	    if (n) {
		Regexp r(n->name(),s_extended,s_insensitive);
		String val;
		if (r.startsWith("${")) {
		    // handle special matching by param ${paramname}regexp
		    int p = r.find('}');
		    if (p < 3) {
			Debug("RegexRoute",DebugWarn,"Invalid parameter match '%s' in rule #%u in context '%s'",
			    r.c_str(),i+1,context.c_str());
			continue;
		    }
		    val = r.substr(2,p-2);
		    r = r.substr(p+1);
		    val.trimBlanks();
		    r.trimBlanks();
		    if (val.null() || r.null()) {
			Debug("RegexRoute",DebugWarn,"Missing parameter or rule in rule #%u in context '%s'",
			    i+1,context.c_str());
			continue;
		    }
		    DDebug("RegexRoute",DebugAll,"Using message parameter '%s'",
			val.c_str());
		    val = msg.getValue(val);
		}
		else if (r.startsWith("$(")) {
		    // handle special matching by param $(function)regexp
		    int p = r.find(')');
		    if (p < 3) {
			Debug("RegexRoute",DebugWarn,"Invalid function match '%s' in rule #%u in context '%s'",
			    r.c_str(),i+1,context.c_str());
			continue;
		    }
		    val = r.substr(0,p+1);
		    r = r.substr(p+1);
		    r.trimBlanks();
		    if (r.null()) {
			Debug("RegexRoute",DebugWarn,"Missing rule in rule #%u in context '%s'",
			    i+1,context.c_str());
			continue;
		    }
		    DDebug("RegexRoute",DebugAll,"Using function '%s'",
			val.c_str());
		    msg.replaceParams(val);
		    replaceFuncs(val);
		}
		else
		    val = str;
		val.trimBlanks();

		bool doMatch = true;
		if (r.endsWith("^")) {
		    // reverse match on final ^ (makes no sense in a regexp)
		    doMatch = false;
		    r = r.substr(0,r.length()-1);
		}
		if (val.matches(r) == doMatch) {
		    val = val.replaceMatches(*n);
		    if (val.startSkip("echo") || val.startSkip("output")) {
			// special case: display the line but don't set params
			msg.replaceParams(val);
			replaceFuncs(val);
			Output("%s",val.safe());
			continue;
		    }
		    else if (val.startSkip("enqueue")) {
			// special case: enqueue a new message
			if (val && (val[0] != ';')) {
			    Message* m = new Message("");
			    // parameters are set in the new message
			    setMessage(msg,val,m);
			    val.trimBlanks();
			    if (val) {
				*m = val;
				m->userData(msg.userData());
				NDebug("RegexRoute",DebugAll,"Enqueueing new message '%s' by rule #%u '%s' in context '%s'",
				    val.c_str(),i+1,n->name().c_str(),context.c_str());
				Engine::enqueue(m);
			    }
			    else
				m->destruct();
			}
			continue;
		    }
		    setMessage(msg,val);
		    val.trimBlanks();
		    if (val.null()) {
			// special case: do nothing on empty target
			continue;
		    }
		    else if (val == "return") {
			NDebug("RegexRoute",DebugAll,"Returning false from context '%s'", context.c_str());
			return false;
		    }
		    else if (val.startSkip("goto") || val.startSkip("jump")) {
			NDebug("RegexRoute",DebugAll,"Jumping to context '%s' by rule #%u '%s'",
			    val.c_str(),i+1,n->name().c_str());
			return oneContext(msg,str,val,ret,depth+1);
		    }
		    else if (val.startSkip("include") || val.startSkip("call")) {
			NDebug("RegexRoute",DebugAll,"Including context '%s' by rule #%u '%s'",
			    val.c_str(),i+1,n->name().c_str());
			if (oneContext(msg,str,val,ret,depth+1)) {
			    DDebug("RegexRoute",DebugAll,"Returning true from context '%s'", context.c_str());
			    return true;
			}
		    }
		    else if (val.startSkip("match") || val.startSkip("newmatch")) {
			if (!val.null()) {
			    NDebug("RegexRoute",DebugAll,"Setting match string '%s' by rule #%u '%s' in context '%s'",
				val.c_str(),i+1,n->name().c_str(),context.c_str());
			    str = val;
			}
		    }
		    else if (val.startSkip("rename")) {
			if (!val.null()) {
			    NDebug("RegexRoute",DebugAll,"Renaming message '%s' to '%s' by rule #%u '%s' in context '%s'",
				msg.c_str(),val.c_str(),i+1,n->name().c_str(),context.c_str());
			    msg = val;
			}
		    }
		    else {
			DDebug("RegexRoute",DebugAll,"Returning '%s' for '%s' in context '%s' by rule #%u '%s'",
			    val.c_str(),str.c_str(),context.c_str(),i+1,n->name().c_str());
			ret = val;
			return true;
		    }
		}
	    }
	}
    }
    DDebug("RegexRoute",DebugAll,"Returning false at end of context '%s'", context.c_str());
    return false;
}
	
bool RouteHandler::received(Message &msg)
{
    u_int64_t tmr = Time::now();
    String called(msg.getValue("called"));
    if (called.null())
	return false;
    const char *context = msg.getValue("context","default");
    Lock lock(s_mutex);
    if (oneContext(msg,called,context,msg.retValue())) {
	Debug(DebugInfo,"Routing call to '%s' in context '%s' via '%s' in " FMT64 " usec",
	    called.c_str(),context,msg.retValue().c_str(),Time::now()-tmr);
	return true;
    }
    Debug(DebugInfo,"Could not route call to '%s' in context '%s', wasted " FMT64 " usec",
	called.c_str(),context,Time::now()-tmr);
    return false;
};
		    
class PrerouteHandler : public MessageHandler
{
public:
    PrerouteHandler(int prio)
	: MessageHandler("call.preroute",prio) { }
    virtual bool received(Message &msg);
};

bool PrerouteHandler::received(Message &msg)
{
    u_int64_t tmr = Time::now();
    // return immediately if there is already a context
    if (msg.getValue("context"))
	return false;

    String caller(msg.getValue("caller"));
    if (caller.null())
	return false;

    String ret;
    Lock lock(s_mutex);
    if (oneContext(msg,caller,"contexts",ret)) {
	Debug(DebugInfo,"Classifying caller '%s' in context '%s' in " FMT64 " usec",
	    caller.c_str(),ret.c_str(),Time::now()-tmr);
	msg.addParam("context",ret);
	return true;
    }
    Debug(DebugInfo,"Could not classify call from '%s', wasted " FMT64 " usec",
	caller.c_str(),Time::now()-tmr);
    return false;
};
		    
class GenericHandler : public MessageHandler
{
public:
    GenericHandler(const char* name, int prio)
	: MessageHandler(name,prio)
	{
	    Debug(DebugAll,"Installing generic handler for '%s' prio %d [%p]",c_str(),prio,this);
	    s_extra.append(this);
	}
    ~GenericHandler()
	{ s_extra.remove(this,false); }
    virtual bool received(Message &msg);
};

bool GenericHandler::received(Message &msg)
{
    DDebug(DebugAll,"Handling message '%s' [%p]",c_str(),this);
    String what(*this);
    Lock lock(s_mutex);
    return oneContext(msg,what,*this,msg.retValue());
}

class RegexRoutePlugin : public Plugin
{
public:
    RegexRoutePlugin();
    virtual void initialize();
private:
    void initVars(NamedList* sect);
    MessageHandler *m_preroute, *m_route;
    bool m_first;
};

RegexRoutePlugin::RegexRoutePlugin()
    : m_preroute(0), m_route(0), m_first(true)
{
    Output("Loaded module RegexRoute");
}

void RegexRoutePlugin::initVars(NamedList* sect)
{
    if (!sect)
	return;
    unsigned int len = sect->length();
    for (unsigned int i=0; i<len; i++) {
	NamedString* n = sect->getParam(i);
	if (n)
	    s_vars.setParam(n->name(),*n);
    }
}

void RegexRoutePlugin::initialize()
{
    Output("Initializing module RegexRoute");
    Lock lock(s_mutex);
    s_cfg = Engine::configFile("regexroute");
    s_cfg.load();
    s_extended = s_cfg.getBoolValue("priorities","extended",false);
    s_insensitive = s_cfg.getBoolValue("priorities","insensitive",false);
    TelEngine::destruct(m_preroute);
    TelEngine::destruct(m_route);
    s_extra.clear();
    unsigned priority = s_cfg.getIntValue("priorities","preroute",100);
    if (priority) {
	m_preroute = new PrerouteHandler(priority);
	Engine::install(m_preroute);
    }
    priority = s_cfg.getIntValue("priorities","route",100);
    if (priority) {
	m_route = new RouteHandler(priority);
	Engine::install(m_route);
    }
    NamedList* l = s_cfg.getSection("extra");
    if (l) {
	unsigned int len = l->length();
	for (unsigned int i=0; i<len; i++) {
	    NamedString* n = l->getParam(i);
	    if (n)
		Engine::install(new GenericHandler(n->name(),n->toInteger()));
	}
    }
    if (m_first) {
	m_first = false;
	initVars(s_cfg.getSection("$once"));
    }
    initVars(s_cfg.getSection("$init"));
}

INIT_PLUGIN(RegexRoutePlugin);

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */
