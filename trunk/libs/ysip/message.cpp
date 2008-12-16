/**
 * message.cpp
 * Yet Another SIP Stack
 * This file is part of the YATE Project http://YATE.null.ro 
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

#include <yatesip.h>
#include "util.h"

#include <string.h>
#include <stdlib.h>


using namespace TelEngine;

SIPMessage::SIPMessage(const SIPMessage& original)
    : version(original.version), method(original.method), uri(original.uri),
      code(original.code), reason(original.reason),
      body(0), m_ep(0),
      m_valid(original.isValid()), m_answer(original.isAnswer()),
      m_outgoing(original.isOutgoing()), m_ack(original.isACK()),
      m_cseq(-1)
{
    DDebug(DebugAll,"SIPMessage::SIPMessage(&%p) [%p]",
	&original,this);
    if (original.body)
	setBody(original.body->clone());
    setParty(original.getParty());
    bool via1 = true;
    const ObjList* l = &original.header;
    for (; l; l = l->next()) {
	const MimeHeaderLine* hl = static_cast<MimeHeaderLine*>(l->get());
	if (!hl)
	    continue;
	// CSeq must not be copied, a new one will be built by complete()
	if (hl->name() &= "CSeq")
	    continue;
	MimeHeaderLine* nl = hl->clone();
	// this is a new transaction so let complete() add randomness
	if (via1 && (nl->name() &= "Via")) {
	    via1 = false;
	    nl->delParam("branch");
	}
	addHeader(nl);
    }
}

SIPMessage::SIPMessage(const char* _method, const char* _uri, const char* _version)
    : version(_version), method(_method), uri(_uri), code(0),
      body(0), m_ep(0), m_valid(true),
      m_answer(false), m_outgoing(true), m_ack(false), m_cseq(-1)
{
    DDebug(DebugAll,"SIPMessage::SIPMessage('%s','%s','%s') [%p]",
	_method,_uri,_version,this);
}

SIPMessage::SIPMessage(SIPParty* ep, const char* buf, int len)
    : code(0), body(0), m_ep(ep), m_valid(false),
      m_answer(false), m_outgoing(false), m_ack(false), m_cseq(-1)
{
    DDebug(DebugInfo,"SIPMessage::SIPMessage(%p,%d) [%p]\n------\n%s------",
	buf,len,this,buf);
    if (m_ep)
	m_ep->ref();
    if (!(buf && *buf)) {
	Debug(DebugWarn,"Empty message text in [%p]",this);
	return;
    }
    if (len < 0)
	len = ::strlen(buf);
    m_valid = parse(buf,len);
}

SIPMessage::SIPMessage(const SIPMessage* message, int _code, const char* _reason)
    : code(_code), body(0),
      m_ep(0), m_valid(false),
      m_answer(true), m_outgoing(true), m_ack(false), m_cseq(-1)
{
    DDebug(DebugAll,"SIPMessage::SIPMessage(%p,%d,'%s') [%p]",
	message,_code,_reason,this);
    if (!_reason)
	_reason = lookup(code,SIPResponses,"Unknown Reason Code");
    reason = _reason;
    if (!(message && message->isValid()))
	return;
    m_ep = message->getParty();
    if (m_ep)
	m_ep->ref();
    version = message->version;
    uri = message->uri;
    method = message->method;
    copyAllHeaders(message,"Via");
    copyAllHeaders(message,"Record-Route");
    copyHeader(message,"From");
    copyHeader(message,"To");
    copyHeader(message,"Call-ID");
    copyHeader(message,"CSeq");
    m_valid = true;
}

SIPMessage::SIPMessage(const SIPMessage* original, const SIPMessage* answer)
    : method("ACK"), code(0),
      body(0), m_ep(0), m_valid(false),
      m_answer(false), m_outgoing(true), m_ack(true), m_cseq(-1)
{
    DDebug(DebugAll,"SIPMessage::SIPMessage(%p,%p) [%p]",original,answer,this);
    if (!(original && original->isValid()))
	return;
    m_ep = original->getParty();
    if (m_ep)
	m_ep->ref();
    version = original->version;
    uri = original->uri;
    copyAllHeaders(original,"Via");
    MimeHeaderLine* hl = const_cast<MimeHeaderLine*>(getHeader("Via"));
    if (!hl) {
	String tmp;
	tmp << version << "/" << getParty()->getProtoName();
	tmp << " " << getParty()->getLocalAddr() << ":" << getParty()->getLocalPort();
	hl = new MimeHeaderLine("Via",tmp);
	header.append(hl);
    }
    if (answer && (answer->code == 200) && (original->method &= "INVITE")) {
	String tmp("z9hG4bK");
	tmp << (int)::random();
	hl->setParam("branch",tmp);
	const MimeHeaderLine* co = answer->getHeader("Contact");
	if (co) {
	    uri = *co;
	    Regexp r("^[^<]*<\\([^>]*\\)>.*$");
	    if (uri.matches(r))
		uri = uri.matchString(1);
	}
	// new transaction - get/apply routeset unless INVITE already knew it
	if (!original->getHeader("Route")) {
	    ObjList* routeset = answer->getRoutes();
	    addRoutes(routeset);
	    TelEngine::destruct(routeset);
	}
    }
    copyAllHeaders(original,"Route");
    copyHeader(original,"From");
    copyHeader(original,"To");
    copyHeader(original,"Call-ID");
    String tmp;
    tmp << original->getCSeq() << " " << method;
    addHeader("CSeq",tmp);
    copyHeader(original,"Max-Forwards");
    copyAllHeaders(original,"Contact");
    copyAllHeaders(original,"Authorization");
    copyAllHeaders(original,"Proxy-Authorization");
    copyHeader(original,"User-Agent");
    m_valid = true;
}

SIPMessage::~SIPMessage()
{
    DDebug(DebugAll,"SIPMessage::~SIPMessage() [%p]",this);
    m_valid = false;
    setParty();
    setBody();
}

void SIPMessage::complete(SIPEngine* engine, const char* user, const char* domain, const char* dlgTag)
{
    DDebug(engine,DebugAll,"SIPMessage::complete(%p,'%s','%s','%s')%s%s%s [%p]",
	engine,user,domain,dlgTag,
	isACK() ? " ACK" : "",
	isOutgoing() ? " OUT" : "",
	isAnswer() ? " ANS" : "",
	this);
    if (!engine)
	return;

    // don't complete incoming messages
    if (!isOutgoing())
	return;

    if (!getParty()) {
	engine->buildParty(this);
	if (!getParty()) {
	    Debug(engine,DebugGoOn,"Could not complete party-less SIP message [%p]",this);
	    return;
	}
    }

    // only set the dialog tag on ACK
    if (isACK()) {
	MimeHeaderLine* hl = const_cast<MimeHeaderLine*>(getHeader("To"));
	if (dlgTag && hl && !hl->getParam("tag"))
	    hl->setParam("tag",dlgTag);
	return;
    }

    if (!domain)
	domain = getParty()->getLocalAddr();

    MimeHeaderLine* hl = const_cast<MimeHeaderLine*>(getHeader("Via"));
    if (!hl) {
	String tmp;
	tmp << version << "/" << getParty()->getProtoName();
	tmp << " " << getParty()->getLocalAddr() << ":" << getParty()->getLocalPort();
	hl = new MimeHeaderLine("Via",tmp);
	if (!(isAnswer() || isACK()))
	    hl->setParam("rport");
	header.append(hl);
    }
    if (!(isAnswer() || hl->getParam("branch"))) {
	String tmp("z9hG4bK");
	tmp << (int)::random();
	hl->setParam("branch",tmp);
    }
    if (isAnswer()) {
	hl->setParam("received",getParty()->getPartyAddr());
	hl->setParam("rport",String(getParty()->getPartyPort()));
    }

    if (!isAnswer()) {
	hl = const_cast<MimeHeaderLine*>(getHeader("From"));
	if (!hl) {
	    String tmp = "<sip:";
	    if (user)
		tmp << user << "@";
	    tmp << domain << ">";
	    hl = new MimeHeaderLine("From",tmp);
	    header.append(hl);
	}
	if (!hl->getParam("tag"))
	    hl->setParam("tag",String((int)::random()));
    }

    hl = const_cast<MimeHeaderLine*>(getHeader("To"));
    if (!(isAnswer() || hl)) {
	String tmp;
	tmp << "<" << uri << ">";
	hl = new MimeHeaderLine("To",tmp);
	header.append(hl);
    }
    if (hl && dlgTag && !hl->getParam("tag"))
	hl->setParam("tag",dlgTag);

    if (!(isAnswer() || getHeader("Call-ID"))) {
	String tmp;
	tmp << (int)::random() << "@" << domain;
	addHeader("Call-ID",tmp);
    }

    if (!(isAnswer() || getHeader("CSeq"))) {
	String tmp;
	m_cseq = engine->getNextCSeq();
	tmp << m_cseq << " " << method;
	addHeader("CSeq",tmp);
    }

    const char* info = isAnswer() ? "Server" : "User-Agent";
    if (!(getHeader(info) || engine->getUserAgent().null()))
	addHeader(info,engine->getUserAgent());

    // keep 100 answers short - they are hop to hop anyway
    if (isAnswer() && (code == 100))
	return;

    if (!(isAnswer() || getHeader("Max-Forwards"))) {
	String tmp(engine->getMaxForwards());
	addHeader("Max-Forwards",tmp);
    }

    if ((method == "INVITE") && !getHeader("Contact")) {
	// automatically add a contact field to (re)INVITE and its answers
	String tmp(user);
	if (!tmp) {
	    tmp = uri;
	    Regexp r(":\\([^:@]*\\)@");
	    tmp.matches(r);
	    tmp = tmp.matchString(1);
	}
	if (tmp) {
	    tmp = "<sip:" + tmp; 
	    tmp << "@" << getParty()->getLocalAddr() ;
	    tmp << ":" << getParty()->getLocalPort() << ">";
	    addHeader("Contact",tmp);
	}
    }

    if (!getHeader("Allow"))
	addHeader("Allow",engine->getAllowed());
}

bool SIPMessage::copyHeader(const SIPMessage* message, const char* name, const char* newName)
{
    const MimeHeaderLine* hl = message ? message->getHeader(name) : 0;
    if (hl) {
	header.append(hl->clone(newName));
	return true;
    }
    return false;
}

int SIPMessage::copyAllHeaders(const SIPMessage* message, const char* name, const char* newName)
{
    if (!(message && name && *name))
	return 0;
    int c = 0;
    const ObjList* l = &message->header;
    for (; l; l = l->next()) {
	const MimeHeaderLine* hl = static_cast<const MimeHeaderLine*>(l->get());
	if (hl && (hl->name() &= name)) {
	    ++c;
	    header.append(hl->clone(newName));
	}
    }
    return c;
}

bool SIPMessage::parseFirst(String& line)
{
    XDebug(DebugAll,"SIPMessage::parse firstline= '%s'",line.c_str());
    if (line.null())
	return false;
    Regexp r("^\\([Ss][Ii][Pp]/[0-9]\\.[0-9]\\+\\)[[:space:]]\\+\\([0-9][0-9][0-9]\\)[[:space:]]\\+\\(.*\\)$");
    if (line.matches(r)) {
	// Answer: <version> <code> <reason-phrase>
	m_answer = true;
	version = line.matchString(1).toUpper();
	code = line.matchString(2).toInteger();
	reason = line.matchString(3);
	DDebug(DebugAll,"got answer version='%s' code=%d reason='%s'",
	    version.c_str(),code,reason.c_str());
    }
    else {
	r = "^\\([[:alpha:]]\\+\\)[[:space:]]\\+\\([^[:space:]]\\+\\)[[:space:]]\\+\\([Ss][Ii][Pp]/[0-9]\\.[0-9]\\+\\)$";
	if (line.matches(r)) {
	    // Request: <method> <uri> <version>
	    m_answer = false;
	    method = line.matchString(1).toUpper();
	    uri = line.matchString(2);
	    version = line.matchString(3).toUpper();
	    DDebug(DebugAll,"got request method='%s' uri='%s' version='%s'",
		method.c_str(),uri.c_str(),version.c_str());
	    if (method == "ACK")
		m_ack = true;
	}
	else {
	    Debug(DebugAll,"Invalid SIP line '%s'",line.c_str());
	    return false;
	}
    }
    return true;
}

bool SIPMessage::parse(const char* buf, int len)
{
    DDebug(DebugAll,"SIPMessage::parse(%p,%d) [%p]",buf,len,this);
    String* line = 0;
    while (len > 0) {
	line = MimeBody::getUnfoldedLine(buf,len);
	if (!line->null())
	    break;
	// Skip any initial empty lines
	TelEngine::destruct(line);
    }
    if (!line)
	return false;
    if (!parseFirst(*line)) {
	line->destruct();
	return false;
    }
    line->destruct();
    int clen = -1;
    while (len > 0) {
	line = MimeBody::getUnfoldedLine(buf,len);
	if (line->null()) {
	    // Found end of headers
	    line->destruct();
	    break;
	}
	int col = line->find(':');
	if (col <= 0) {
	    line->destruct();
	    return false;
	}
	String name = line->substr(0,col);
	name.trimBlanks();
	if (name.null()) {
	    line->destruct();
	    return false;
	}
	name = uncompactForm(name);
	*line >> ":";
	line->trimBlanks();
	XDebug(DebugAll,"SIPMessage::parse header='%s' value='%s'",name.c_str(),line->c_str());

	if ((name &= "WWW-Authenticate") ||
	    (name &= "Proxy-Authenticate") ||
	    (name &= "Authorization") ||
	    (name &= "Proxy-Authorization"))
	    header.append(new MimeAuthLine(name,*line));
	else
	    header.append(new MimeHeaderLine(name,*line));

	if ((clen < 0) && (name &= "Content-Length"))
	    clen = line->toInteger(-1,10);
	else if ((m_cseq < 0) && (name &= "CSeq")) {
	    String seq = *line;
	    seq >> m_cseq;
	    if (m_answer) {
		seq.trimBlanks().toUpper();
		method = seq;
	    }
	}
	line->destruct();
    }
    if (clen >= 0) {
	if (clen > len)
	    Debug("SIPMessage",DebugMild,"Content length is %d but only %d in buffer",clen,len);
	else if (clen < len) {
	    DDebug("SIPMessage",DebugInfo,"Got %d garbage bytes after content",len - clen);
	    len = clen;
	}
    }
    const MimeHeaderLine* cType = getHeader("Content-Type");
    if (cType)
	body = MimeBody::build(buf,len,*cType);
    // Move extra Content- header lines to body
    if (body) {
	ListIterator iter(header);
	for (GenObject* o = 0; (o = iter.get());) {
	    MimeHeaderLine* line = static_cast<MimeHeaderLine*>(o);
	    if (!line->startsWith("Content-",false,true) || (*line &= "Content-Length"))
		continue;
	    // Delete Content-Type and move all other lines to body
	    bool delobj = (line == cType);
	    header.remove(o,delobj);
	    if (!delobj)
		body->appendHdr(line);
	}
    }
    DDebug(DebugAll,"SIPMessage::parse %d header lines, body %p",
	header.count(),body);
    return true;
}

SIPMessage* SIPMessage::fromParsing(SIPParty* ep, const char* buf, int len)
{
    SIPMessage* msg = new SIPMessage(ep,buf,len);
    if (msg->isValid())
	return msg;
    DDebug("SIPMessage",DebugInfo,"Invalid message");
    msg->destruct();
    return 0;
}

const MimeHeaderLine* SIPMessage::getHeader(const char* name) const
{
    if (!(name && *name))
	return 0;
    const ObjList* l = &header;
    for (; l; l = l->next()) {
	const MimeHeaderLine* t = static_cast<const MimeHeaderLine*>(l->get());
	if (t && (t->name() &= name))
	    return t;
    }
    return 0;
}

const MimeHeaderLine* SIPMessage::getLastHeader(const char* name) const
{
    if (!(name && *name))
	return 0;
    const MimeHeaderLine* res = 0;
    const ObjList* l = &header;
    for (; l; l = l->next()) {
	const MimeHeaderLine* t = static_cast<const MimeHeaderLine*>(l->get());
	if (t && (t->name() &= name))
	    res = t;
    }
    return res;
}

void SIPMessage::clearHeaders(const char* name)
{
    if (!(name && *name))
	return;
    ObjList* l = &header;
    while (l) {
	const MimeHeaderLine* t = static_cast<const MimeHeaderLine*>(l->get());
	if (t && (t->name() &= name))
	    l->remove();
	else
	    l = l->next();
    }
}

int SIPMessage::countHeaders(const char* name) const
{
    if (!(name && *name))
	return 0;
    int res = 0;
    const ObjList* l = &header;
    for (; l; l = l->next()) {
	const MimeHeaderLine* t = static_cast<const MimeHeaderLine*>(l->get());
	if (t && (t->name() &= name))
	    ++res;
    }
    return res;
}

const NamedString* SIPMessage::getParam(const char* name, const char* param) const
{
    const MimeHeaderLine* hl = getHeader(name);
    return hl ? hl->getParam(param) : 0;
}

const String& SIPMessage::getHeaderValue(const char* name) const
{
    const MimeHeaderLine* hl = getHeader(name);
    return hl ? *static_cast<const String*>(hl) : String::empty();
}

const String& SIPMessage::getParamValue(const char* name, const char* param) const
{
    const NamedString* ns = getParam(name,param);
    return ns ? *static_cast<const String*>(ns) : String::empty();
}

const String& SIPMessage::getHeaders() const
{
    if (isValid() && m_string.null()) {
	if (isAnswer())
	    m_string << version << " " << code << " " << reason << "\r\n";
	else
	    m_string << method << " " << uri << " " << version << "\r\n";

	const ObjList* l = &header;
	for (; l; l = l->next()) {
	    MimeHeaderLine* t = static_cast<MimeHeaderLine*>(l->get());
	    if (t) {
		t->buildLine(m_string);
		m_string << "\r\n";
	    }
	}
    }
    return m_string;
}

const DataBlock& SIPMessage::getBuffer() const
{
    if (isValid() && m_data.null()) {
	m_data.assign((void*)(getHeaders().c_str()),getHeaders().length());
	if (body) {
	    String s;
	    body->buildHeaders(s);
	    s << "Content-Length: " << body->getBody().length() << "\r\n\r\n";
	    m_data += s;
	}
	else
	    m_data += "Content-Length: 0\r\n\r\n";
	if (body)
	    m_data += body->getBody();
#ifdef DEBUG
	if (debugAt(DebugInfo)) {
	    String buf((char*)m_data.data(),m_data.length());
	    Debug(DebugInfo,"SIPMessage::getBuffer() [%p]\n------\n%s------",
		this,buf.c_str());
	}
#endif
    }
    return m_data;
}

void SIPMessage::setBody(MimeBody* newbody)
{
    if (newbody == body)
	return;
    TelEngine::destruct(body);
    body = newbody;
}

void SIPMessage::setParty(SIPParty* ep)
{
    if (ep == m_ep)
	return;
    if (m_ep)
	m_ep->deref();
    m_ep = ep;
    if (m_ep)
	m_ep->ref();
}

MimeAuthLine* SIPMessage::buildAuth(const String& username, const String& password,
    const String& meth, const String& uri, bool proxy) const
{
    const char* hdr = proxy ? "Proxy-Authenticate" : "WWW-Authenticate";
    const ObjList* l = &header;
    for (; l; l = l->next()) {
	const MimeAuthLine* t = YOBJECT(MimeAuthLine,l->get());
	if (t && (t->name() &= hdr) && (*t &= "Digest")) {
	    String nonce(t->getParam("nonce"));
	    MimeHeaderLine::delQuotes(nonce);
	    if (nonce.null())
		continue;
	    String realm(t->getParam("realm"));
	    MimeHeaderLine::delQuotes(realm);
	    int par = uri.find(';');
	    String msguri = uri.substr(0,par);
	    String response;
	    SIPEngine::buildAuth(username,realm,password,nonce,meth,msguri,response);
	    MimeAuthLine* auth = new MimeAuthLine(proxy ? "Proxy-Authorization" : "Authorization","Digest");
	    auth->setParam("username",MimeHeaderLine::quote(username));
	    auth->setParam("realm",MimeHeaderLine::quote(realm));
	    auth->setParam("nonce",MimeHeaderLine::quote(nonce));
	    auth->setParam("uri",MimeHeaderLine::quote(msguri));
	    auth->setParam("response",MimeHeaderLine::quote(response));
	    auth->setParam("algorithm","MD5");
	    // copy opaque data as-is, only if present
	    const NamedString* opaque = t->getParam("opaque");
	    if (opaque)
		auth->setParam(opaque->name(),*opaque);
	    return auth;
	}
    }
    return 0;
}

MimeAuthLine* SIPMessage::buildAuth(const SIPMessage& original) const
{
    if (original.getAuthUsername().null())
	return 0;
    return buildAuth(original.getAuthUsername(),original.getAuthPassword(),
	original.method,original.uri,(code == 407));
}

ObjList* SIPMessage::getRoutes() const
{
    ObjList* list = 0;
    const ObjList* l = &header;
    for (; l; l = l->next()) {
	const MimeHeaderLine* h = YOBJECT(MimeHeaderLine,l->get());
	if (h && (h->name() &= "Record-Route")) {
	    int p = 0;
	    while (p >= 0) {
		MimeHeaderLine* line = 0;
		int s = MimeHeaderLine::findSep(*h,',',p);
		String tmp;
		if (s < 0) {
		    if (p)
			tmp = h->substr(p);
		    else
			line = new MimeHeaderLine(*h,"Route");
		    p = -1;
		}
		else {
		    if (s > p)
			tmp = h->substr(p,s-p);
		    p = s + 1;
		}
		tmp.trimBlanks();
		if (tmp)
		    line = new MimeHeaderLine("Route",tmp);
		if (!line)
		    continue;
		if (!list)
		    list = new ObjList;
		if (isAnswer())
		    // route set learned from an answer, reverse order
		    list->insert(line);
		else
		    // route set learned from a request, preserve order
		    list->append(line);
	    }
	}
    }
    return list;
}

void SIPMessage::addRoutes(const ObjList* routes)
{
    if (isAnswer() || !routes)
	return;
    MimeHeaderLine* hl = YOBJECT(MimeHeaderLine,routes->get());
    if (hl) {
	// check if first route is to a RFC 2543 proxy
	String tmp = *hl;
	Regexp r("<\\([^>]\\+\\)>");
	if (tmp.matches(r))
	    tmp = tmp.matchString(1);
	if (tmp.find(";lr") < 0) {
	    // prepare a new final route
	    hl = new MimeHeaderLine("Route","<" + uri + ">");
	    // set the first route as Request-URI and then skip it
	    uri = tmp;
	    routes = routes->next();
	}
	else
	    hl = 0;
    }

    // add (remaining) routes
    for (; routes; routes = routes->next()) {
	const MimeHeaderLine* h = YOBJECT(MimeHeaderLine,routes->get());
	if (h)
	    addHeader(h->clone());
    }

    // if first route was to a RFC 2543 proxy add the old Request-URI
    if (hl)
	addHeader(hl);
}

SIPDialog::SIPDialog()
{
}

SIPDialog::SIPDialog(const SIPDialog& original)
    : String(original),
      localURI(original.localURI), localTag(original.localTag),
      remoteURI(original.remoteURI), remoteTag(original.remoteTag)
{
    DDebug("SIPDialog",DebugAll,"callid '%s' local '%s;tag=%s' remote '%s;tag=%s' [%p]",
	c_str(),localURI.c_str(),localTag.c_str(),remoteURI.c_str(),remoteTag.c_str(),this);
}

SIPDialog& SIPDialog::operator=(const SIPDialog& original)
{
    String::operator=(original);
    localURI = original.localURI;
    localTag = original.localTag;
    remoteURI = original.remoteURI;
    remoteTag = original.remoteTag;
    DDebug("SIPDialog",DebugAll,"callid '%s' local '%s;tag=%s' remote '%s;tag=%s' [%p]",
	c_str(),localURI.c_str(),localTag.c_str(),remoteURI.c_str(),remoteTag.c_str(),this);
    return *this;
}

SIPDialog& SIPDialog::operator=(const String& callid)
{
    String::operator=(callid);
    localURI.clear();
    localTag.clear();
    remoteURI.clear();
    remoteTag.clear();
    DDebug("SIPDialog",DebugAll,"callid '%s' local '%s;tag=%s' remote '%s;tag=%s' [%p]",
	c_str(),localURI.c_str(),localTag.c_str(),remoteURI.c_str(),remoteTag.c_str(),this);
    return *this;
}

SIPDialog::SIPDialog(const SIPMessage& message)
    : String(message.getHeaderValue("Call-ID"))
{
    Regexp r("<\\([^>]\\+\\)>");
    bool local = message.isOutgoing() ^ message.isAnswer();
    const MimeHeaderLine* hl = message.getHeader(local ? "From" : "To");
    localURI = hl;
    if (localURI.matches(r))
        localURI = localURI.matchString(1);
    if (hl)
	localTag = hl->getParam("tag");
    hl = message.getHeader(local ? "To" : "From");
    remoteURI = hl;
    if (remoteURI.matches(r))
        remoteURI = remoteURI.matchString(1);
    if (hl)
	remoteTag = hl->getParam("tag");
    DDebug("SIPDialog",DebugAll,"callid '%s' local '%s;tag=%s' remote '%s;tag=%s' [%p]",
	c_str(),localURI.c_str(),localTag.c_str(),remoteURI.c_str(),remoteTag.c_str(),this);
}

SIPDialog& SIPDialog::operator=(const SIPMessage& message)
{
    const char* cid = message.getHeaderValue("Call-ID");
    if (cid)
	String::operator=(cid);
    Regexp r("<\\([^>]\\+\\)>");
    bool local = message.isOutgoing() ^ message.isAnswer();
    const MimeHeaderLine* hl = message.getHeader(local ? "From" : "To");
    localURI = hl;
    if (localURI.matches(r))
        localURI = localURI.matchString(1);
    if (hl)
	localTag = hl->getParam("tag");
    hl = message.getHeader(local ? "To" : "From");
    remoteURI = hl;
    if (remoteURI.matches(r))
        remoteURI = remoteURI.matchString(1);
    if (hl)
	remoteTag = hl->getParam("tag");
    DDebug("SIPDialog",DebugAll,"callid '%s' local '%s;tag=%s' remote '%s;tag=%s' [%p]",
	c_str(),localURI.c_str(),localTag.c_str(),remoteURI.c_str(),remoteTag.c_str(),this);
    return *this;
}

bool SIPDialog::operator==(const SIPDialog& other) const
{
    return
	String::operator==(other) &&
	localURI == other.localURI &&
	localTag == other.localTag &&
	remoteURI == other.remoteURI &&
	remoteTag == other.remoteTag;
}

bool SIPDialog::operator!=(const SIPDialog& other) const
{
    return !operator==(other);
}

/* vi: set ts=8 sw=4 sts=4 noet: */