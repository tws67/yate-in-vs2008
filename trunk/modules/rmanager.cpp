/**
 * rmanager.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * This module gets the messages from YATE out so anyone can use an
 * administrating interface.
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

#include <yatengine.h>

#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

#ifdef HAVE_COREDUMPER
#include <google/coredumper.h>
#endif

using namespace TelEngine;
namespace { // anonymous

typedef struct {
    const char* name;
    const char* args;
    const char** more;
    const char* desc;
} CommandInfo;

static const char* s_bools[] =
{
    "on",
    "off",
    "enable",
    "disable",
    "true",
    "false",
    0
};

static const char* s_level[] =
{
    "level",
    "on",
    "off",
    "enable",
    "disable",
    "true",
    "false",
    0
};

static const char* s_oview[] =
{
    "overview",
    0
};

static const char* s_dall[] =
{
    "all",
    0
};

static const char* s_rnow[] =
{
    "now",
    0
};

static const CommandInfo s_cmdInfo[] =
{
    { "quit", 0, 0, "Disconnect this control session from Yate" },
    { "help", "[command]", 0, "Provide help on all or given command" },
    { "status", "[overview] [modulename]", s_oview, "Shows status of all or selected modules or channels" },
    { "uptime", 0, 0, "Show information on how long Yate has run" },
    { "echo", "[on|off]", s_bools, "Show or turn remote echo on or off" },
    { "machine", "[on|off]", s_bools, "Show or turn machine output mode on or off" },
    { "output", "[on|off]", s_bools, "Show or turn local output on or off" },
    { "color", "[on|off]", s_bools, "Show status or turn local colorization on or off" },
    { "auth", "password", 0, "Authenticate so you can access priviledged commands" },
    { "debug", "[module] [level|on|off]", s_level, "Show or change debugging level globally or per module" },
#ifdef HAVE_COREDUMPER
    { "coredump", "[filename]", 0, "Dumps memory image of running Yate to a file" },
#endif
    { "drop", "{chan|*|all} [reason]", s_dall, "Drops one or all active calls" },
    { "call", "chan target", 0, "Execute an outgoing call" },
    { "reload", 0, 0, "Reloads module configuration files" },
    { "restart", "[now]", s_rnow, "Restarts the engine if executing supervised" },
    { "stop", "[exitcode]", 0, "Stops the engine with optionally provided exit code" },
    { 0, 0, 0, 0 }
};

static void completeWords(String& str, const char** list, const char* partial = 0)
{
    if (!list)
	return;
    for (; *list; list++) {
	String tmp = *list;
	if (null(partial) || tmp.startsWith(partial))
	    str.append(tmp,"\t");
    }
}

static Configuration s_cfg;

static bool s_output = false;

/* I need this here because i'm gonna use it in both classes */
Socket s_sock;
Mutex s_mutex(true);

//we gonna create here the list with all the new connections.
static ObjList connectionlist;
    
class RManagerThread : public Thread
{
public:
    RManagerThread() : Thread("RManager Listener") { }
    virtual void run();
private:
};

class Connection : public GenObject, public Thread
{
public:
    Connection(Socket* sock, const char* addr);
    ~Connection();

    virtual void run();
    bool processTelnetChar(unsigned char c);
    bool processChar(unsigned char c);
    bool processLine(const char *line);
    bool autoComplete();
    void errorBeep();
    void clearLine();
    void writeStr(const char *str, int len = -1);
    void writeDebug(const char *str, int level);
    void writeStr(const Message &msg, bool received);
    inline void writeStr(const String &s)
	{ writeStr(s.safe(),s.length()); }
    inline const String& address() const
	{ return m_address; }
    static Connection *checkCreate(Socket* sock, const char* addr = 0);
private:
    bool m_auth;
    bool m_debug;
    bool m_output;
    bool m_colorize;
    bool m_machine;
    Socket* m_socket;
    unsigned char m_lastch;
    unsigned char m_escmode;
    bool m_echoing;
    bool m_beeping;
    String m_buffer;
    String m_address;
    String m_lastcmd;
};

class RManager : public Plugin
{
public:
    RManager();
    ~RManager();
    virtual void initialize();
    virtual bool isBusy() const;
private:
    bool m_first;
};

class RHook : public MessagePostHook
{
public:
    virtual void dispatched(const Message& msg, bool handled);
};

static void dbg_remote_func(const char *buf, int level)
{
    s_mutex.lock();
    ObjList *p = &connectionlist;
    for (; p; p=p->next()) {
	Connection *con = static_cast<Connection *>(p->get());
	if (con)
	    con->writeDebug(buf,level);
    }
    s_mutex.unlock();
}

void RManagerThread::run()
{
    for (;;)
    {
	Thread::msleep(10,true);
	SocketAddr sa;
	Socket* as = s_sock.accept(sa);
	if (!as) {
	    if (!s_sock.canRetry())
		Debug("RManager",DebugWarn, "Accept error: %s", strerror(s_sock.error()));
	    continue;
	} else {
	    String addr(sa.host());
	    addr << ":" << sa.port();
	    if (!Connection::checkCreate(as,addr))
		Debug("RManager",DebugWarn,"Connection rejected for %s",addr.c_str());
	}
    }
}

Connection *Connection::checkCreate(Socket* sock, const char* addr)
{
    if (!sock)
	return 0;
    if (!sock->valid()) {
	delete sock;
	return 0;
    }
    // should check IP address here
    Connection *conn = new Connection(sock,addr);
    if (conn->error()) {
	conn->destruct();
	return 0;
    }
    conn->startup();
    return conn;
}

Connection::Connection(Socket* sock, const char* addr)
    : Thread("RManager Connection"),
      m_auth(false), m_debug(false), m_output(s_output), m_colorize(false), m_machine(false),
      m_socket(sock), m_lastch(0), m_escmode(0), m_echoing(false), m_beeping(false),
      m_address(addr)
{
    s_mutex.lock();
    connectionlist.append(this);
    s_mutex.unlock();
}

Connection::~Connection()
{
    m_debug = false;
    m_output = false;
    s_mutex.lock();
    connectionlist.remove(this,false);
    s_mutex.unlock();
    Output("Closing connection to %s",m_address.c_str());
    delete m_socket;
    m_socket = 0;
}

void Connection::run()
{
    if (!m_socket)
	return;
    if (!m_socket->setBlocking(false)) {
	Debug("RManager",DebugGoOn, "Failed to set tcp socket to nonblocking mode: %s",strerror(m_socket->error()));
	return;
    }

    // For the sake of responsiveness try to turn off the tcp assembly timer
    int arg = 1;
    if (s_cfg.getBoolValue("general","interactive",false) &&
	!m_socket->setOption(SOL_SOCKET, TCP_NODELAY, &arg, sizeof(arg)))
	Debug("RManager",DebugMild, "Failed to set tcp socket to TCP_NODELAY mode: %s", strerror(m_socket->error()));

    Output("Remote connection from %s",m_address.c_str());
    m_auth = !s_cfg.getValue("general","password");
    const char *hdr = s_cfg.getValue("general","header","YATE (http://YATE.null.ro) ready.");
    if (hdr) {
	writeStr(hdr);
	writeStr("\r\n");
	hdr = 0;
    }
    if (s_cfg.getBoolValue("general","telnet",true)) {
	// WILL SUPPRESS GO AHEAD, WILL ECHO - and enough BS and blanks to hide them
	writeStr("\377\373\003\377\373\001\b\b\b\b\b\b      \b\b\b\b\b\b");
    }
    unsigned char buffer[128];
    for (;;) {
	Thread::check();
	bool readok = false;
	bool error = false;
	if (m_socket->select(&readok,0,&error,10000)) {
	    // rearm the error beep
	    m_beeping = false;
	    if (error) {
		Debug("RManager",DebugInfo,"Socket exception condition on %d",m_socket->handle());
		return;
	    }
	    if (!readok)
		continue;
	    int readsize = m_socket->readData(buffer,sizeof(buffer));
	    if (!readsize) {
		Debug("RManager",DebugInfo,"Socket condition EOF on %d",m_socket->handle());
		return;
	    }
	    else if (readsize > 0) {
		for (int i = 0; i < readsize; i++)
		    if (processTelnetChar(buffer[i]))
			return;
	    }
	    else if (!m_socket->canRetry()) {
		Debug("RManager",DebugWarn,"Socket read error %d on %d",errno,m_socket->handle());
		return;
	    }
	}
	else if (!m_socket->canRetry()) {
	    Debug("RManager",DebugWarn,"socket select error %d on %d",errno,m_socket->handle());
	    return;
	}
    }	
}

// generates a beep - just one per processed buffer
void Connection::errorBeep()
{
    if (m_beeping)
	return;
    m_beeping = true;
    writeStr("\a");
}

// clears the current line to end
void Connection::clearLine()
{
    writeStr("\r\033[K\r");
}

// process incoming telnet characters
bool Connection::processTelnetChar(unsigned char c)
{
    XDebug("RManager",DebugInfo,"char=0x%02X '%s%c'",
	c,(c >= ' ') ? "" : "^", (c >= ' ') ? c : c+0x40);
    if (m_lastch == 255) {
	m_lastch = 0;
	switch (c) {
	    case 241: // NOP
		return false;
	    case 243: // BREAK
		c = 0x1C;
		break;
	    case 244: // IP
		c = 0x03;
		break;
	    case 247: // EC
		c = 0x08;
		break;
	    case 248: // EL
		c = 0x15;
		break;
	    case 251: // WILL
	    case 252: // WON'T
	    case 253: // DO
	    case 254: // DON'T
		m_lastch = c;
		return false;
	    case 255: // IAC
		break;
	    default:
		Debug("RManager",DebugMild,"Unsupported telnet command %u (0x%02X)",c,c);
		return false;
	}
    }
    else if (m_lastch) {
	DDebug("RManager",DebugMild,"Command %u param %u",m_lastch,c);
	switch (m_lastch) {
	    case 251: // WILL
		switch (c) {
		    case 1: // ECHO
			m_echoing = false;
			writeStr("\377\374\001"); // WON'T ECHO
			break;
		}
	    case 252: // WON'T
		break;
	    case 253: // DO
		switch (c) {
		    case 1: // ECHO
			m_echoing = true;
			writeStr("\377\373\001"); // WILL ECHO
			break;
		    case 3: // SUPPRESS GO AHEAD
			writeStr("\377\373\003"); // WILL SUPPRESS GO AHEAD
			break;
		    case 18: // LOGOUT
			writeStr("\377\373\022"); // WILL LOGOUT
			return true;
		    default:
			writeStr("\377\374"); // WON'T ...
			writeStr((const char*)&c,1);
			break;
		}
		break;
	    case 254: // DON'T
		switch (c) {
		    case 1:
			m_echoing = false;
			writeStr("\377\374\001"); // WON'T ECHO
			break;
		}
		break;
	}
	m_lastch = 0;
	return false;
    }
    else if (c == 255) {
	m_lastch = c;
	return false;
    }
    return processChar(c);
}

// process incoming terminal characters
bool Connection::processChar(unsigned char c)
{
    switch (c) {
	case '\0':
	    m_escmode = 0;
	    return false;
	case 0x1B: // ESC
	    m_escmode = c;
	    return false;
	case '\n':
	    m_escmode = 0;
	    if (m_buffer.null())
		return false;
	case '\r':
	    m_escmode = 0;
	    if (m_echoing)
		writeStr("\r\n");
	    if (processLine(m_buffer))
		return true;
	    m_buffer.clear();
	    return false;
	case 0x03: // ^C, BREAK
	    m_escmode = 0;
	    writeStr("^C\r\n");
	    return true;
	case 0x04: // ^D, UNIX EOF
	    m_escmode = 0;
	    if (m_buffer) {
		errorBeep();
		return false;
	    }
	    return processLine("quit");
	case 0x1C: // ^backslash
	    if (m_buffer)
		break;
	    return processLine("reload");
	case 0x05: // ^E
	    m_escmode = 0;
	    m_echoing = !m_echoing;
	    return false;
	case 0x0C: // ^L
	    if (!m_echoing)
		break;
	    writeStr("\033[H\033[2J");
	    writeStr(m_buffer);
	    return false;
	case 0x12: // ^R
	    if (!m_echoing)
		break;
	    clearLine();
	    writeStr(m_buffer);
	    return false;
	case 0x15: // ^U
	    if (m_buffer.null())
		break;
	    m_escmode = 0;
	    m_buffer.clear();
	    if (m_echoing)
		clearLine();
	    return false;
	case 0x17: // ^W
	    if (m_buffer.null())
		errorBeep();
	    else {
		int i = m_buffer.length()-1;
		for (; i > 0; i--)
		    if (m_buffer[i] != ' ')
			break;
		for (; i > 0; i--)
		    if (m_buffer[i] == ' ') {
			i++;
			break;
		    }
		m_escmode = 0;
		m_buffer.assign(m_buffer,i);
		if (m_echoing) {
		    clearLine();
		    writeStr(m_buffer);
		}
	    }
	    return false;
	case 0x7F: // DEL
	case 0x08: // ^H, BACKSPACE
	    if (m_buffer.null()) {
		errorBeep();
		return false;
	    }
	    m_escmode = 0;
	    m_buffer.assign(m_buffer,m_buffer.length()-1);
	    if (m_echoing)
		writeStr("\b \b");
	    return false;
	case 0x09: // ^I, TAB
	    m_escmode = 0;
	    if (m_buffer.null())
		return processLine("help");
	    if (!autoComplete())
		errorBeep();
	    return false;
    }
    if (m_escmode) {
	switch (c) {
	    case '[':
	    case '0':
	    case '1':
	    case '2':
	    case '3':
	    case '4':
	    case '5':
	    case '6':
	    case '7':
	    case '8':
	    case '9':
	    case ';':
	    case 'O':
		m_escmode = c;
		return false;
	}
	unsigned char last = m_escmode;
	m_escmode = 0;
	DDebug("RManager",DebugInfo,"ANSI '%s%c' last '%s%c'",
	    (c >= ' ') ? "" : "^", (c >= ' ') ? c : c+0x40,
	    (last >= ' ') ? "" : "^", (last >= ' ') ? last : last+0x40
	);
	switch (c) {
	    case 'A': // Up arrow
	    case 'B': // Down arrow
		if (m_lastcmd.null()) {
		    errorBeep();
		    return false;
		}
		else {
		    String str = m_lastcmd;
		    if (m_buffer)
			m_lastcmd = m_buffer;
		    m_buffer = str;
		}
		clearLine();
		writeStr(m_buffer);
		return false;
	}
	c = 0;
    }
    if (c < ' ') {
	errorBeep();
	return false;
    }
    if (m_echoing && (c == ' ')) {
	if (m_buffer.null() || m_buffer.endsWith(" ")) {
	    errorBeep();
	    return false;
	}
    }
    m_buffer += (char)c;
    if (m_echoing)
	writeStr((char*)&c,1);
    return false;
}

// perform auto-completion of partial line
bool Connection::autoComplete()
{
    DDebug("RManager",DebugInfo,"autoComplete = '%s'",m_buffer.c_str());
    Message m("engine.command");
    m.addParam("partial",m_buffer);
    String partLine;
    String partWord;
    int keepLen = m_buffer.length();
    // find start and end of word
    int i = keepLen-1;
    if (m_buffer[i] == ' ') {
	// we are at start of new word
	partLine = m_buffer;
	partLine.trimBlanks();
	if (partLine == "?")
	    partLine = "help";
	const CommandInfo* info = s_cmdInfo;
	bool help = (partLine == "help");
	for (; info->name; info++) {
	    if (help)
		m.retValue().append(info->name,"\t");
	    else if (partLine == info->name) {
		completeWords(m.retValue(),info->more);
		break;
	    }
	}
    }
    else {
	// we are completing a started word
	for (; i >= 0; i--) {
	    if (m_buffer[i] == ' ')
		break;
	}
	i++;
	keepLen = i;
	partLine = m_buffer.substr(0,i);
	partWord = m_buffer.substr(i);
	partLine.trimBlanks();
	if (partLine == "?")
	    partLine = "help";
	else if (partLine.null() && (partWord == "?"))
	    partWord = "help";
	if (partLine.null()) {
	    m.addParam("complete","command");
	    const CommandInfo* info = s_cmdInfo;
	    for (; info->name; info++) {
		String cmd = info->name;
		if (cmd.startsWith(partWord))
		    m.retValue().append(cmd,"\t");
	    }
	}
	else {
	    const CommandInfo* info = s_cmdInfo;
	    bool help = (partLine == "help");
	    if (help)
		m.addParam("complete","command");
	    for (; info->name; info++) {
		if (help) {
		    String arg = info->name;
		    if (arg.startsWith(partWord))
			m.retValue().append(arg,"\t");
		}
		else if (partLine == info->name) {
		    completeWords(m.retValue(),info->more,partWord);
		    break;
		}
	    }
	}
    }
    if (partLine) {
	if (partLine == "status overview")
	    partLine = "status";
	m.addParam("partline",partLine);
    }
    if (partWord)
	m.addParam("partword",partWord);
    if ((partLine == "status") || (partLine == "debug") || (partLine == "drop"))
	m.setParam("complete","channels");
    Regexp r("^debug [^ ]\\+$");
    if (r.matches(partLine))
	completeWords(m.retValue(),s_level,partWord);
    Engine::dispatch(m);
    if (m.retValue().null())
	return false;
    if (m.retValue().find('\t') < 0) {
	m_buffer = m_buffer.substr(0,keepLen)+m.retValue()+" ";
	clearLine();
	writeStr(m_buffer);
	return true;
    }
    // more options returned - list them and display the prompt again
    writeStr("\r\n");
    writeStr(m.retValue());
    bool first = true;
    String maxMatch;
    ObjList* l = m.retValue().split('\t');
    for (ObjList* p = l; p; p = p->next()) {
    	String* s = static_cast<String*>(p->get());
	if (!s)
	    continue;
	if (first) {
	    first = false;
	    maxMatch = *s;
	}
	else {
	    while (maxMatch && !s->startsWith(maxMatch)) {
		maxMatch.assign(maxMatch,maxMatch.length()-1);
	    }
	}
    }
    TelEngine::destruct(l);
    m_buffer += maxMatch.substr(partWord.length());
    writeStr("\r\n");
    writeStr(m_buffer);
    return true;
}

// execute received input line
bool Connection::processLine(const char *line)
{
    DDebug("RManager",DebugInfo,"processLine = '%s'",line);
    String str(line);
    str.trimBlanks();
    if (str.null())
	return false;

    m_lastcmd = str;

    if (str.startSkip("status"))
    {
	Message m("engine.status");
	if (str.startSkip("overview"))
	    m.addParam("details",String::boolText(false));
	if (str.null() || (str == "rmanager"))
	    m.retValue() << "name=rmanager,type=misc;conn=" << connectionlist.count() << "\r\n";
	if (!str.null()) {
	    m.addParam("module",str);
	    str = ":" + str;
	}
	Engine::dispatch(m);
	str = "%%+status" + str + "\r\n";
	str << m.retValue() << "%%-status\r\n";
	writeStr(str);
	return false;
    }
    else if (str.startSkip("echo"))
    {
	str >> m_echoing;
	str = "Remote echo: ";
	str += (m_echoing ? "on\r\n" : "off\r\n");
	writeStr(str);
	return false;
    }
    else if (str.startSkip("machine"))
    {
	str >> m_machine;
	str = "Machine mode: ";
	str += (m_machine ? "on\r\n" : "off\r\n");
	writeStr(str);
	return false;
    }
    else if (str.startSkip("output"))
    {
	str >> m_output;
	str = "Output mode: ";
	str += (m_output ? "on\r\n" : "off\r\n");
	writeStr(str);
	return false;
    }
    else if (str.startSkip("color"))
    {
	str >> m_colorize;
	str = "Colorized output: ";
	str += (m_colorize ? "yes\r\n" : "no\r\n");
	writeStr(str);
	return false;
    }
    else if (str.startSkip("uptime"))
    {
	str.clear();
	u_int32_t t = SysUsage::secRunTime();
	if (m_machine) {
	    str << "%%=uptime:" << (unsigned int)t;
	    (str << ":").append(SysUsage::runTime(SysUsage::UserTime));
	    (str << ":").append(SysUsage::runTime(SysUsage::KernelTime));
	}
	else {
	    char buf[64];
	    ::sprintf(buf,"%u:%02u:%02u (%u)",t / 3600,(t / 60) % 60,t % 60,t);
	    str << "Uptime: " << buf;
	    (str << " user: ").append(SysUsage::runTime(SysUsage::UserTime));
	    (str << " kernel: ").append(SysUsage::runTime(SysUsage::KernelTime));
	}
	str << "\r\n";
	writeStr(str);
	return false;
    }
    else if (str.startSkip("quit"))
    {
	writeStr(m_machine ? "%%=quit\r\n" : "Goodbye!\r\n");
	return true;
    }
    else if (str.startSkip("help") || str.startSkip("?"))
    {
	Message m("engine.help");
	if (str)
	{
	    const CommandInfo* info = s_cmdInfo;
	    for (; info->name; info++) {
		if (str == info->name) {
		    str = "  ";
		    str << info->name;
		    if (info->args)
			str << " " << info->args;
		    str << "\r\n" << info->desc << "\r\n";
		    writeStr(str);
		    return false;
		}
	    }
	    m.addParam("line",str);
	    if (Engine::dispatch(m))
		writeStr(m.retValue());
	    else
		writeStr("No help for '"+str+"'\r\n");
	}
	else
	{
	    m.retValue() = "Available commands:\r\n";
	    const CommandInfo* info = s_cmdInfo;
	    for (; info->name; info++) {
		m.retValue() << "  " << info->name;
		if (info->args)
		    m.retValue() << " " << info->args;
		m.retValue() << "\r\n";
	    }
	    Engine::dispatch(m);
	    writeStr(m.retValue());
	}
	return false;
    }
    else if (str.startSkip("auth"))
    {
	if (m_auth) {
	    writeStr(m_machine ? "%%=auth:success\r\n" : "You are already authenticated!\r\n");
	    return false;
	}
	if (str == s_cfg.getValue("general","password")) {
	    Output("Authenticated connection %s",m_address.c_str());
	    m_auth = true;
	    writeStr(m_machine ? "%%=auth:success\r\n" : "Authenticated successfully!\r\n");
	}
	else
	    writeStr(m_machine ? "%%=auth:fail=badpass\r\n" : "Bad authentication password!\r\n");
	return false;
    }
    if (!m_auth) {
	writeStr(m_machine ? "%%=*:fail=noauth\r\n" : "Not authenticated!\r\n");
	return false;
    }
    if (str.startSkip("drop"))
    {
	String reason;
	int pos = str.find(' ');
	if (pos > 0) {
	    reason = str.substr(pos+1);
	    str = str.substr(0,pos);
	}
	if (str.null()) {
	    writeStr(m_machine ? "%%=drop:fail=noarg\r\n" : "You must specify what connection to drop!\r\n");
	    return false;
	}
	Message m("call.drop");
	bool all = false;
	if (str == "*" || str == "all") {
	    all = true;
	    str = "all calls";
	}
	else
	    m.addParam("id",str); 
	if (reason)
	    m.addParam("reason",reason);
	if (Engine::dispatch(m))
	    str = (m_machine ? "%%=drop:success:" : "Dropped ") + str + "\r\n";
	else if (all)
	    str = (m_machine ? "%%=drop:unknown:" : "Tried to drop ") + str + "\r\n";
	else
	    str = (m_machine ? "%%=drop:fail:" : "Could not drop ") + str + "\r\n";
	writeStr(str);
    }
    else if (str.startSkip("call"))
    {
	int pos = str.find(' ');
	if (pos <= 0) {
	    writeStr(m_machine ? "%%=call:fail=noarg\r\n" : "You must specify source and target!\r\n");
	    return false;
	}
	String target = str.substr(pos+1);
	Message m("call.execute");
	m.addParam("callto",str.substr(0,pos));
	m.addParam((target.find('/') > 0) ? "direct" : "target",target);

	if (Engine::dispatch(m)) {
	    String id(m.getValue("id"));
	    if (m_machine)
		str = "%%=call:success:" + id + ":" + str + "\r\n";
	    else
		str = "Calling '" + id + "' " + str + "\r\n";
	}
	else
	    str = (m_machine ? "%%=call:fail:" : "Could not call ") + str + "\r\n";
	writeStr(str);
    }
    else if (str.startSkip("debug"))
    {
	if (str.startSkip("level")) {
	    int dbg = debugLevel();
	    str >> dbg;
	    dbg = debugLevel(dbg);
	}
	else if (str.isBoolean()) {
	    str >> m_debug;
	    if (m_debug)
		Debugger::enableOutput(true);
	}
	else if (str) {
	    String l;
	    int pos = str.find(' ');
	    if (pos > 0) {
		l = str.substr(pos+1);
		str = str.substr(0,pos);
		str.trimBlanks();
	    }
	    if (str.null()) {
		writeStr(m_machine ? "%%=debug:fail=noarg\r\n" : "You must specify debug module name!\r\n");
		return false;
	    }
	    Message m("engine.debug");
	    m.addParam("module",str);
	    if (l)
		m.addParam("line",l);
	    if (Engine::dispatch(m))
		writeStr(m.retValue());
	    else
		writeStr((m_machine ? "%%=debug:fail:" : "Cannot set debug: ") + str + " " + l + "\r\n");
	    return false;
	}
	if (m_machine) {
	    str = "%%=debug:level=";
	    str << debugLevel() << ":local=" << m_debug << "\r\n";
	}
	else {
	    str = "Debug level: ";
	    str << debugLevel() << " local: " << (m_debug ? "on\r\n" : "off\r\n");
	}
	writeStr(str);
    }
#ifdef HAVE_COREDUMPER
    else if (str.startSkip("coredump"))
    {
	if (str.null())
	    (str << "core.yate-" << ::getpid() << "-").append(SysUsage::runTime());
	s_mutex.lock();
	int err = 0;
	for (int i = 0; i < 4; i++) {
	    if (!WriteCoreDump(str)) {
		err = 0;
		break;
	    }
	    err = errno;
	    switch (err) {
		case EINTR:
		case EAGAIN:
		case ECHILD:
		    continue;
	    }
	    break;
	}
	if (err) {
	    str = "Failed to dump core: ";
	    str << ::strerror(err) << " (" << err << ")\r\n";
	}
	else
	    str = "Dumped core to: " + str + "\r\n";
	s_mutex.unlock();
	writeStr(str);
    }
#endif
    else if (str.startSkip("reload"))
    {
	writeStr(m_machine ? "%%=reload\r\n" : "Reinitializing...\r\n");
	Engine::init();
    }
    else if (str.startSkip("restart"))
    {
	bool gracefull = (str != "now");
	bool ok = Engine::restart(0,gracefull);
	if (ok) {
	    if (m_machine) {
		writeStr("%%=restart\r\n");
		return gracefull;
	    }
	    writeStr(gracefull ? "Restart scheduled - please disconnect\r\n" : "Engine restarting - bye!\r\n");
	}
	else
	    writeStr(m_machine ? "%%=restart:fail\r\n" : "Cannot restart - no supervisor or already shutting down\r\n");
    }
    else if (str.startSkip("stop"))
    {
	unsigned code = 0;
	str >> code;
	code &= 0xff;
	writeStr(m_machine ? "%%=shutdown\r\n" : "Engine shutting down - bye!\r\n");
	Engine::halt(code);
    }
    else
    {
	Message m("engine.command");
	m.addParam("line",str);
	if (Engine::dispatch(m))
	    writeStr(m.retValue());
	else
	    writeStr((m_machine ? "%%=syntax:" : "Cannot understand: ") + str + "\r\n");
    }
    return false;
}

// dump encoded messages after processing, only in machine mode
void Connection::writeStr(const Message &msg, bool received)
{
    if (!m_machine)
	return;
    String s = msg.encode(received,"");
    s << "\r\n";
    writeStr(s.c_str());
}

// write debugging messages to the remote console
void Connection::writeDebug(const char *str, int level)
{
    if (null(str))
	return;
    if (m_debug || (m_output && (level < 0))) {
	const char* col = m_colorize ? debugColor(level) : 0;
	if (col)
	    writeStr(col,::strlen(col));
	int len = ::strlen(str);
	for (; len > 0; len--) {
	    if ((unsigned char)str[len-1] >= ' ')
		break;
	}
	writeStr(str,len);
	writeStr("\r\n",2);
	if (col)
	    col = debugColor(-2);
	if (col)
	    writeStr(col,::strlen(col));
    }
}

// write arbitrary string to the remote console
void Connection::writeStr(const char *str, int len)
{
    if (len < 0)
	len = ::strlen(str);
    else if (len == 0)
	return;
    if (int written = m_socket->writeData(str,len) != len) {
	Debug("RManager",DebugInfo,"Socket %d wrote only %d out of %d bytes",m_socket->handle(),written,len);
	// Destroy the thread, will kill the connection
	cancel();
    }
}


void RHook::dispatched(const Message& msg, bool handled)
{
    s_mutex.lock();
    ObjList *p = &connectionlist;
    for (; p; p=p->next()) {
	Connection *con = static_cast<Connection *>(p->get());
	if (con)
	    con->writeStr(msg,handled);
    }
    s_mutex.unlock();
};


RManager::RManager()
    : m_first(true)
{
    Output("Loaded module RManager");
    Debugger::setIntOut(dbg_remote_func);
}

RManager::~RManager()
{
    Output("Unloading module RManager");
    s_sock.terminate();
    Debugger::setIntOut(0);
}

bool RManager::isBusy() const
{
    return (connectionlist.count() != 0);
}

void RManager::initialize()
{
    Output("Initializing module RManager");
    s_cfg = Engine::configFile("rmanager");
    s_cfg.load();
    s_output = s_cfg.getBoolValue("general","output",false);

    if (s_sock.valid())
	return;

    // check configuration
    int port = s_cfg.getIntValue("general","port",5038);
    const char *host = c_safe(s_cfg.getValue("general","addr","127.0.0.1"));
    if (!(port && *host))
	return;

    s_sock.create(AF_INET, SOCK_STREAM);
    if (!s_sock.valid()) {
	Debug("RManager",DebugGoOn,"Unable to create the listening socket: %s",strerror(s_sock.error()));
	return;
    }

    if (!s_sock.setBlocking(false)) {
	Debug("RManager",DebugGoOn, "Failed to set listener to nonblocking mode: %s",strerror(s_sock.error()));
	return;
    }

    const int reuseFlag = 1;
    s_sock.setOption(SOL_SOCKET,SO_REUSEADDR,&reuseFlag,sizeof(reuseFlag));

    SocketAddr sa(AF_INET);
    sa.host(host);
    sa.port(port);
    s_sock.setReuse();
    if (!s_sock.bind(sa)) {
	Debug("RManager",DebugGoOn,"Failed to bind to %s:%u : %s",sa.host().c_str(),sa.port(),strerror(s_sock.error()));
	s_sock.terminate();
	return;
    }
    if (!s_sock.listen(2)) {
	Debug("RManager",DebugGoOn,"Unable to listen on socket: %s", strerror(s_sock.error()));
	s_sock.terminate();
	return;
    }
    
    // don't bother to install handlers until we are listening
    if (m_first) {
	m_first = false;
	Engine::self()->setHook(new RHook);
	RManagerThread *mt = new RManagerThread;
	mt->startup();
    }
}

INIT_PLUGIN(RManager);

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */