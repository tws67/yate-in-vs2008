/**
 * tonegen.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Tones generator
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

#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 40ms silence, 120ms tone, 40ms silence, total 200ms - slow but safe
#define DTMF_LEN 960
#define DTMF_GAP 320

using namespace TelEngine;
namespace { // anonymous

static ObjList tones;
static ObjList datas;

typedef struct {
    int nsamples;
    const short* data;
} Tone;

typedef struct {
    const Tone* tone;
    const char* name;
    const char* alias;
} ToneDesc;

class ToneData : public GenObject
{
public:
    ToneData(const char* desc);
    inline ToneData(int f1, int f2 = 0, bool modulated = false)
	: m_f1(f1), m_f2(f2), m_mod(modulated), m_data(0)
	{ }
    inline ToneData(const ToneData& original)
	: m_f1(original.f1()), m_f2(original.f2()),
	  m_mod(original.modulated()), m_data(0)
	{ }
    virtual ~ToneData();
    inline int f1() const
	{ return m_f1; }
    inline int f2() const
	{ return m_f2; }
    inline bool modulated() const
	{ return m_mod; }
    inline bool valid() const
	{ return m_f1 != 0; }
    inline bool equals(int f1, int f2) const
	{ return (m_f1 == f1) && (m_f2 == f2); }
    inline bool equals(const ToneData& other) const
	{ return (m_f1 == other.f1()) && (m_f2 == other.f2()); }
    const short* data();
    static ToneData* getData(const char* desc);
private:
    bool parse(const char* desc);
    int m_f1;
    int m_f2;
    bool m_mod;
    const short* m_data;
};

class ToneSource : public ThreadedSource
{
public:
    virtual void destroyed();
    virtual void run();
    inline const String& name()
	{ return m_name; }
    bool startup();
    static ToneSource* getTone(String& tone);
    static const ToneDesc* getBlock(String& tone);
    static Tone* buildCadence(const String& desc);
    static Tone* buildDtmf(const String& dtmf, int len = DTMF_LEN, int gap = DTMF_GAP);
protected:
    ToneSource(const ToneDesc* tone = 0);
    virtual void zeroRefs();
    String m_name;
    const Tone* m_tone;
    int m_repeat;
private:
    DataBlock m_data;
    unsigned m_brate;
    unsigned m_total;
    u_int64_t m_time;
};

class TempSource : public ToneSource
{
public:
    TempSource(String& desc, DataBlock* rawdata);
    virtual ~TempSource();
protected:
    virtual void cleanup();
private:
    Tone* m_single;
    DataBlock* m_rawdata;                // Raw linear data to be sent
};

class ToneChan : public Channel
{
public:
    ToneChan(String& tone);
    ~ToneChan();
};

class AttachHandler : public MessageHandler
{
public:
    AttachHandler() : MessageHandler("chan.attach") { }
    virtual bool received(Message& msg);
};

class ToneGenDriver : public Driver
{
public:
    ToneGenDriver();
    ~ToneGenDriver();
    virtual void initialize();
    virtual bool msgExecute(Message& msg, String& dest);
protected:
    void statusModule(String& str);
    void statusParams(String& str);
private:
    AttachHandler* m_handler;
};

INIT_PLUGIN(ToneGenDriver);

// 421.052Hz (19 samples @ 8kHz) sine wave, pretty close to standard 425Hz
static const short tone421hz[] = {
    19,
    3246, 6142, 8371, 9694, 9965, 9157, 7357, 4759, 1645,
    -1645, -4759, -7357, -9157, -9965, -9694, -8371, -6142, -3246,
    0 };

// 1000Hz (8 samples @ 8kHz) standard digital milliwatt
static const short tone1000hz[] = {
    8,
    8828, 20860, 20860, 8828,
    -8828, -20860, -20860, -8828
    };

// 941.176Hz (2*8.5 samples @ 8kHz) sine wave, approximates 950Hz
static const short tone941hz[] = {
    17,
    6736, 9957, 7980, 1838, -5623, -9617, -8952, -3614,
    3614, 8952, 9617, 5623, -1838, -7980, -9957, -6736,
    0 };

// 1454.545Hz (2*5.5 samples @ 8kHz) sine wave, approximates 1400Hz
static const short tone1454hz[] = {
    11,
    9096, 7557, -2816, -9898, -5407,
    5407, 9898, 2816, -7557, -9096,
    0 };

// 1777.777Hz (2*4.5 samples @ 8kHz) sine wave, approximates 1800Hz
static const short tone1777hz[] = {
    9,
    9848, 3420, -8659, -6429,
    6429, 8659, -3420, -9848,
    0 };

static const Tone t_dial[] = { { 8000, tone421hz }, { 0, 0 } };

static const Tone t_busy[] = { { 4000, tone421hz }, { 4000, 0 }, { 0, 0 } };

static const Tone t_specdial[] = { { 7600, tone421hz }, { 400, 0 }, { 0, 0 } };

static const Tone t_ring[] = { { 8000, tone421hz }, { 32000, 0 }, { 0, 0 } };

static const Tone t_congestion[] = { { 2000, tone421hz }, { 2000, 0 }, { 0, 0 } };

static const Tone t_outoforder[] = {
    { 800, tone421hz }, { 800, 0 },
    { 800, tone421hz }, { 800, 0 },
    { 800, tone421hz }, { 800, 0 },
    { 1600, tone421hz }, { 1600, 0 },
    { 0, 0 } };

static const Tone t_info[] = {
    { 2640, tone941hz }, { 240, 0 },
    { 2640, tone1454hz }, { 240, 0 },
    { 2640, tone1777hz }, { 8000, 0 },
    { 0, 0 } };

static const Tone t_mwatt[] = { { 8000, tone1000hz }, { 0, 0 } };

static const Tone t_silence[] = { { 8000, 0 }, { 0, 0 } };

static const Tone t_noise[] = { { 2000, ToneData::getData("noise")->data() }, { 0, 0 } };

#define MAKE_DTMF(s) { \
    { DTMF_GAP, 0 }, \
    { DTMF_LEN, ToneData::getData(s)->data() }, \
    { DTMF_GAP, 0 }, \
    { 0, 0 } \
}
static const Tone t_dtmf[][4] = {
    MAKE_DTMF("1336+941"),
    MAKE_DTMF("1209+697"),
    MAKE_DTMF("1336+697"),
    MAKE_DTMF("1477+697"),
    MAKE_DTMF("1209+770"),
    MAKE_DTMF("1336+770"),
    MAKE_DTMF("1477+770"),
    MAKE_DTMF("1209+852"),
    MAKE_DTMF("1336+852"),
    MAKE_DTMF("1477+852"),
    MAKE_DTMF("1209+941"),
    MAKE_DTMF("1477+941"),
    MAKE_DTMF("1633+697"),
    MAKE_DTMF("1633+770"),
    MAKE_DTMF("1633+852"),
    MAKE_DTMF("1633+941")
};
#undef MAKE_DTMF

#define MAKE_PROBE(s) { \
    { 8000, ToneData::getData(s)->data() }, \
    { 0, 0 } \
}
static const Tone t_probes[][2] = {
    MAKE_PROBE("2000+125"),
    MAKE_PROBE("2000*125"),
    MAKE_PROBE("2000*1000"),
};
#undef MAKE_PROBE

static const ToneDesc s_desc[] = {
    { t_dial, "dial", "dt" },
    { t_busy, "busy", "bs" },
    { t_ring, "ring", "rt" },
    { t_specdial, "specdial", "sd" },
    { t_congestion, "congestion", "cg" },
    { t_outoforder, "outoforder", "oo" },
    { t_info, "info", "in" },
    { t_mwatt, "milliwatt", "mw" },
    { t_silence, "silence", 0 },
    { t_noise, "noise", "cn" },
    { t_dtmf[0], "dtmf/0", "0" },
    { t_dtmf[1], "dtmf/1", "1" },
    { t_dtmf[2], "dtmf/2", "2" },
    { t_dtmf[3], "dtmf/3", "3" },
    { t_dtmf[4], "dtmf/4", "4" },
    { t_dtmf[5], "dtmf/5", "5" },
    { t_dtmf[6], "dtmf/6", "6" },
    { t_dtmf[7], "dtmf/7", "7" },
    { t_dtmf[8], "dtmf/8", "8" },
    { t_dtmf[9], "dtmf/9", "9" },
    { t_dtmf[10], "dtmf/*", "*" },
    { t_dtmf[11], "dtmf/#", "#" },
    { t_dtmf[12], "dtmf/a", "a" },
    { t_dtmf[13], "dtmf/b", "b" },
    { t_dtmf[14], "dtmf/c", "c" },
    { t_dtmf[15], "dtmf/d", "d" },
    { t_probes[0], "probe/0", "probe" },
    { t_probes[1], "probe/1", 0 },
    { t_probes[2], "probe/2", 0 },
    { 0, 0, 0 }
};

// This function is here mainly to avoid 64bit gcc b0rking optimizations
static unsigned int byteRate(u_int64_t time, unsigned int bytes)
{
    if (!(time && bytes))
	return 0;
    time = Time::now() - time;
    if (!time)
	return 0;
    return (unsigned int)((bytes*(u_int64_t)1000000 + time/2) / time);
}


ToneData::ToneData(const char* desc)
    : m_f1(0), m_f2(0), m_mod(false), m_data(0)
{
    if (!parse(desc)) {
	Debug(&__plugin,DebugWarn,"Invalid tone description '%s'",desc);
	m_f1 = m_f2 = 0;
	m_mod = false;
    }
}

ToneData::~ToneData()
{
    if (m_data) {
	::free((void*)m_data);
	m_data = 0;
    }
}

// a tone data description is something like "425" or "350+440" or "15*2100"
bool ToneData::parse(const char* desc)
{
    if (!desc)
	return false;
    String tmp(desc);
    if (tmp == "noise") {
	m_f1 = -10;
	return true;
    }
    tmp >> m_f1;
    if (!m_f1)
	return false;
    if (m_f1 < -15)
	m_f1 = -15;
    if (tmp) {
	char sep;
	tmp >> sep;
	switch (sep) {
	    case '+':
		break;
	    case '*':
		m_mod = true;
		break;
	    default:
		return false;
	}
	tmp >> m_f2;
	if (!m_f2)
	    return false;
	// order components so we can compare correctly
	if (m_f1 < m_f2) {
	    int t = m_f1;
	    m_f1 = m_f2;
	    m_f2 = t;
	}
    }
    return true;
}

const short* ToneData::data()
{
    if (m_f1 && !m_data) {
	// generate the data on first call
	short len = 8000;
	if (m_f1 < 0) {
	    Debug(&__plugin,DebugAll,"Building comfort noise at level %d",m_f1);
	    // we don't need much memory for noise...
	    len /= 8;
	}
	else if (m_f2)
	    Debug(&__plugin,DebugAll,"Building tone of %d %s %d Hz",
		m_f1,(m_mod ? "modulated by" : "+"),m_f2);
	else {
	    Debug(&__plugin,DebugAll,"Building tone of %d Hz",m_f1);
	    // half the buffer for even frequencies
	    if ((m_f1 & 1) == 0)
		len /= 2;
	}
	short* dat = (short*)::malloc((len+1)*sizeof(short));
	if (!dat) {
	    Debug(&__plugin,DebugGoOn,"ToneData::data() cold not allocate memory for %d elements",len);
	    return 0;
	}
	short* tmp = dat;
	*tmp++ = len;
	if (m_f1 < 0) {
	    int ofs = 65535 >> (-m_f1);
	    int max = 2 * ofs + 1;
	    for (int x = 0; x < len; x++)
		*tmp++ = (short)((::random() % max) - ofs);
	}
	else {
	    double samp = 2*M_PI/8000;
	    for (int x = 0; x < len; x++) {
		double y = ::sin(x*samp*m_f1);
		if (m_f2) {
		    double z = ::sin(x*samp*m_f2);
		    if (m_mod)
			y *= (1+0.5*z);
		    else
			y += z;
		}
		*tmp++ = (short)(y*5000);
	    }
	}
	m_data = dat;
    }
    return m_data;
}

ToneData* ToneData::getData(const char* desc)
{
    ToneData td(desc);
    if (!td.valid())
	return 0;
    ObjList* l = &datas;
    for (; l; l = l->next()) {
	ToneData* d = static_cast<ToneData*>(l->get());
	if (d && d->equals(td))
	    return d;
    }
    ToneData* d = new ToneData(td);
    datas.append(d);
    return d;
}


ToneSource::ToneSource(const ToneDesc* tone)
    : m_tone(0), m_repeat(tone == 0),
      m_data(0,320), m_brate(16000), m_total(0), m_time(0)
{
    if (tone) {
	m_tone = tone->tone;
	m_name = tone->name;
    }
    Debug(&__plugin,DebugAll,"ToneSource::ToneSource(%p) '%s' [%p]",
	tone,m_name.c_str(),this);
    asyncDelete(true);
}

void ToneSource::destroyed()
{
    Debug(&__plugin,DebugAll,"ToneSource::destroyed() '%s' [%p] total=%u stamp=%lu",
	m_name.c_str(),this,m_total,timeStamp());
    ThreadedSource::destroyed();
    if (m_time)
	Debug(&__plugin,DebugInfo,"ToneSource rate=%u b/s",byteRate(m_time,m_total));
}

void ToneSource::zeroRefs()
{
    Debug(&__plugin,DebugAll,"ToneSource::zeroRefs() '%s' [%p]",m_name.c_str(),this);
    __plugin.lock();
    tones.remove(this,false);
    __plugin.unlock();
    ThreadedSource::zeroRefs();
}

bool ToneSource::startup()
{
    DDebug(&__plugin,DebugAll,"ToneSource::startup(\"%s\") tone=%p",m_name.c_str(),m_tone);
    return m_tone && start("ToneSource");
}

const ToneDesc* ToneSource::getBlock(String& tone)
{
    if (tone.trimBlanks().toLower().null())
	return 0;
    const ToneDesc* d = s_desc;
    for (; d->tone; d++) {
	if (tone == d->name)
	    return d;
	if (d->alias && (tone == d->alias)) {
	    tone = d->name;
	    return d;
	}
    }
    return 0;
}

// Build an user defined cadence
Tone* ToneSource::buildCadence(const String& desc)
{
    // TBD
    return 0;
}

// Build a cadence out of DTMFs
Tone* ToneSource::buildDtmf(const String& dtmf, int len, int gap)
{
    if (dtmf.null())
	return 0;
    Tone* tmp = (Tone*)::malloc(2*sizeof(Tone)*(dtmf.length()+1));
    if (!tmp)
	return 0;
    Tone* t = tmp;

    for (unsigned int i = 0; i < dtmf.length(); i++) {
	t->nsamples = gap;
	t->data = 0;
	t++;

	int c = dtmf.at(i);
	if ((c >= '0') && (c <= '9'))
	    c -= '0';
	else if (c == '*')
	    c = 10;
	else if (c == '#')
	    c = 11;
	else if ((c >= 'a') && (c <= 'd'))
	    c -= ('a' - 12);
	else c = -1;

	t->nsamples = len;
	t->data = ((c >= 0) && (c < 16)) ? t_dtmf[c][1].data : 0;
	t++;
    }

    t->nsamples = gap;
    t->data = 0;
    t++;
    t->nsamples = 0;
    t->data = 0;

    return tmp;
}

ToneSource* ToneSource::getTone(String& tone)
{
    const ToneDesc* td = ToneSource::getBlock(tone);
    // tone name is now canonical
    ObjList* l = &tones;
    for (; l; l = l->next()) {
	ToneSource* t = static_cast<ToneSource*>(l->get());
	if (t && (t->name() == tone) && t->ref())
	    return t;
    }
    ToneSource* t = new ToneSource(td);
    tones.append(t);
    t->startup();
    return t;
}

void ToneSource::run()
{
    Debug(&__plugin,DebugAll,"ToneSource::run() [%p]",this);
    u_int64_t tpos = Time::now();
    m_time = tpos;
    int samp = 0; // sample number
    int dpos = 1; // position in data
    const Tone* tone = m_tone;
    int nsam = tone->nsamples;
    if (nsam < 0)
	nsam = -nsam;
    while (alive() && m_tone) {
	Thread::check();
	short *d = (short *) m_data.data();
	for (unsigned int i = m_data.length()/2; i--; samp++,dpos++) {
	    if (samp >= nsam) {
		// go to the start of the next tone
		samp = 0;
		const Tone *otone = tone;
		tone++;
		if (!tone->nsamples) {
		    if ((m_repeat > 0) && !(--m_repeat))
			m_tone = 0;
		    tone = m_tone;
		}
		nsam = tone ? tone->nsamples : 32000;
		if (nsam < 0) {
		    nsam = -nsam;
		    // reset repeat point here
		    m_tone = tone;
		}
		if (tone != otone)
		    dpos = 1;
	    }
	    if (tone && tone->data) {
		if (dpos > tone->data[0])
		    dpos = 1;
		*d++ = tone->data[dpos];
	    }
	    else
		*d++ = 0;
	}
	int64_t dly = tpos - Time::now();
	if (dly > 0) {
	    XDebug(&__plugin,DebugAll,"ToneSource sleeping for " FMT64 " usec",dly);
	    Thread::usleep((unsigned long)dly);
	}
	Forward(m_data,m_total/2);
	m_total += m_data.length();
	tpos += (m_data.length()*(u_int64_t)1000000/m_brate);
    }
    Debug(&__plugin,DebugAll,"ToneSource [%p] end, total=%u (%u b/s)",
	this,m_total,byteRate(m_time,m_total));
    m_time = 0;
}


TempSource::TempSource(String& desc, DataBlock* rawdata)
    : m_single(0), m_rawdata(rawdata)
{
    Debug(&__plugin,DebugAll,"TempSource::TempSource(\"%s\") [%p]",desc.c_str(),this);
    if (desc.null())
	return;
    if (desc.startSkip("*",false))
	m_repeat = 0;
    // Build a source used to send raw linear data
    if (desc == "rawdata") {
	if (!(m_rawdata && m_rawdata->length() >= sizeof(short))) {
	    Debug(&__plugin,DebugNote,
		"TempSource::TempSource(\"%s\") invalid data size=%u [%p]",
		desc.c_str(),m_rawdata?m_rawdata->length():0,this);
	    return;
	}
	m_name = "rawdata";
	m_tone = m_single = (Tone*)::malloc(2*sizeof(Tone));
	m_single[0].nsamples = m_rawdata->length() / sizeof(short);
	m_single[0].data = (short*)m_rawdata->data();
	m_single[1].nsamples = 0;
	m_single[1].data = 0;
	return;
    }
    // try first the named tones
    const ToneDesc* tde = getBlock(desc);
    if (tde) {
	m_tone = tde->tone;
	return;
    }
    // for performance reason accept an entire string of DTMFs
    if (desc.startSkip("dtmfstr/",false)) {
	m_tone = m_single = buildDtmf(desc);
	return;
    }
    // or an entire user defined cadence of tones
    if (desc.startSkip("cadence/",false)) {
	m_tone = m_single = buildCadence(desc);
	return;
    }
    // now try to build a single tone
    ToneData* td = ToneData::getData(desc);
    if (!td)
	return;
    m_single = (Tone*)::malloc(2*sizeof(Tone));
    m_single[0].nsamples = 8000;
    m_single[0].data = td->data();
    m_single[1].nsamples = 0;
    m_single[1].data = 0;
    m_tone = m_single;
}

TempSource::~TempSource()
{
    Debug(&__plugin,DebugAll,"TempSource::~TempSource() [%p]",this);
    if (m_single) {
	::free(m_single);
	m_single = 0;
    }
    TelEngine::destruct(m_rawdata);
}

void TempSource::cleanup()
{
    ToneSource::cleanup();
    deref();
}


ToneChan::ToneChan(String& tone)
    : Channel(__plugin)
{
    Debug(this,DebugAll,"ToneChan::ToneChan(\"%s\") [%p]",tone.c_str(),this);
    // protect the list while the new tone source is added to it
    __plugin.lock();
    ToneSource* t = ToneSource::getTone(tone);
    __plugin.unlock();
    if (t) {
	setSource(t);
	m_address = t->name();
	t->deref();
    }
    else
	Debug(DebugWarn,"No source tone '%s' in ToneChan [%p]",tone.c_str(),this);
}

ToneChan::~ToneChan()
{
    Debug(this,DebugAll,"ToneChan::~ToneChan() %s [%p]",id().c_str(),this);
}

// Get a data block from a binary parameter of msg
DataBlock* getRawData(Message& msg)
{
    NamedString* data = msg.getParam("rawdata");
    if (!data)
	return 0;
    NamedPointer* p = static_cast<NamedPointer*>(data->getObject("NamedPointer"));
    if (!p)
	return 0;
    GenObject* gen = p->userData();
    if (!(gen && gen->getObject("DataBlock")))
	return 0;
    return static_cast<DataBlock*>(p->takeData());
}

bool AttachHandler::received(Message& msg)
{
    String src(msg.getValue("source"));
    if (!src.startSkip("tone/",false))
	src.clear();
    String ovr(msg.getValue("override"));
    if (!ovr.startSkip("tone/",false))
	ovr.clear();
    String repl(msg.getValue("replace"));
    if (!repl.startSkip("tone/",false))
	repl.clear();
    if (src.null() && ovr.null() && repl.null())
	return false;

    DataEndpoint* de = static_cast<DataEndpoint*>(msg.userObject("DataEndpoint"));
    if (!de) {
	CallEndpoint* ch = static_cast<CallEndpoint*>(msg.userObject("CallEndpoint"));
	if (ch)
	    de = ch->setEndpoint();
    }

    if (!de) {
	Debug(DebugWarn,"Tone attach request with no control or data channel!");
	return false;
    }

    // if single attach was requested we can return true if everything is ok
    bool ret = msg.getBoolValue("single");

    Lock lock(__plugin);
    if (src) {
	ToneSource* t = ToneSource::getTone(src);
	if (t) {
	    de->setSource(t);
	    t->deref();
	    msg.clearParam("source");
	}
	else {
	    Debug(DebugWarn,"No source tone '%s' could be attached to %p",src.c_str(),de);
	    ret = false;
	}
    }
    if (ovr) {
	DataConsumer* c = de->getConsumer();
	if (c) {
	    TempSource* t = new TempSource(ovr,getRawData(msg));
	    if (DataTranslator::attachChain(t,c,true) && t->startup())
		msg.clearParam("override");
	    else {
		Debug(DebugWarn,"Override source tone '%s' failed to start [%p]",ovr.c_str(),t);
		ret = false;
	    }
	}
	else {
	    Debug(DebugWarn,"Requested override '%s' to missing consumer of %p",ovr.c_str(),de);
	    ret = false;
	}
    }
    if (repl) {
	DataConsumer* c = de->getConsumer();
	if (c) {
	    TempSource* t = new TempSource(repl,getRawData(msg));
	    if (DataTranslator::attachChain(t,c,false) && t->startup())
		msg.clearParam("replace");
	    else {
		Debug(DebugWarn,"Replacement source tone '%s' failed to start [%p]",repl.c_str(),t);
		ret = false;
	    }
	}
	else {
	    Debug(DebugWarn,"Requested replacement '%s' to missing consumer of %p",repl.c_str(),de);
	    ret = false;
	}
    }
    return ret;
}


bool ToneGenDriver::msgExecute(Message& msg, String& dest)
{
    CallEndpoint* ch = static_cast<CallEndpoint*>(msg.userData());
    if (ch) {
	ToneChan *tc = new ToneChan(dest);
	if (ch->connect(tc,msg.getValue("reason"))) {
	    tc->callConnect(msg);
	    msg.setParam("peerid",tc->id());
	    tc->deref();
	}
	else {
	    tc->destruct();
	    return false;
	}
    }
    else {
	Message m("call.route");
	m.addParam("module",name());
	m.copyParam(msg,"called");
	m.copyParam(msg,"caller");
	m.copyParam(msg,"callername");
	String callto(msg.getValue("direct"));
	if (callto.null()) {
	    const char *targ = msg.getValue("target");
	    if (!targ)
		targ = msg.getValue("called");
	    if (!targ) {
		Debug(DebugWarn,"Tone outgoing call with no target!");
		return false;
	    }
	    callto = msg.getValue("caller");
	    if (callto.null())
		callto << prefix() << dest;
	    m.setParam("called",targ);
	    m.setParam("caller",callto);
	    if ((!Engine::dispatch(m)) || m.retValue().null() || (m.retValue() == "-")) {
		Debug(DebugWarn,"Tone outgoing call but no route!");
		return false;
	    }
	    callto = m.retValue();
	    m.retValue().clear();
	}
	m = "call.execute";
	m.setParam("callto",callto);
	ToneChan *tc = new ToneChan(dest);
	m.setParam("id",tc->id());
	m.userData(tc);
	if (Engine::dispatch(m)) {
	    msg.setParam("id",tc->id());
	    tc->deref();
	    return true;
	}
	Debug(DebugWarn,"Tone outgoing call not accepted!");
	tc->destruct();
	return false;
    }
    return true;
}

void ToneGenDriver::statusModule(String& str)
{
    Module::statusModule(str);
}

void ToneGenDriver::statusParams(String& str)
{
    str << "tones=" << tones.count() << ",chans=" << channels().count();
}

ToneGenDriver::ToneGenDriver()
    : Driver("tone","misc"), m_handler(0)
{
    Output("Loaded module ToneGen");
}

ToneGenDriver::~ToneGenDriver()
{
    Output("Unloading module ToneGen");
    ObjList* l = &channels();
    while (l) {
	ToneChan* t = static_cast<ToneChan *>(l->get());
	if (t)
	    t->disconnect("shutdown");
	if (l->get() == t)
	    l = l->next();
    }
    lock();
    channels().clear();
    tones.clear();
    unlock();
}

void ToneGenDriver::initialize()
{
    Output("Initializing module ToneGen");
    setup(0,true); // no need to install notifications
    Driver::initialize();
    if (!m_handler) {
	m_handler = new AttachHandler;
	Engine::install(m_handler);
	installRelay(Halt);
    }
}

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */
