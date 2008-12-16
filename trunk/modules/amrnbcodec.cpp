/**
 * amrnbcodec.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * AMR narrowband transcoder implemented using 3GPP codec
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2007 Null Team
 * Author: Paul Chitescu
 *
 * AMR codec library by Stanislav Brabec at http://www.penguin.cz/~utx/amr
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

extern "C" {
#include <interf_enc.h>
#include <interf_dec.h>
}
namespace RxTypes {
// There is a conflict between encoder and decoder so insulate in a namespace
#include <sp_dec.h>
};

// IF1/GP3 is Bandwidth-Efficient Mode
// IF2 is Octet-aligned Mode (not supported here)

using namespace TelEngine;
namespace { // anonymous

#define MODNAME "amrnbcodec"

// Transcoding voice size, 20ms of 8kHz slin data
#define SAMPLES_FRAME 160

// Transcoding buffer size, 2 bytes per sample
#define BUFFER_SIZE   (2*SAMPLES_FRAME)

// Maximum compressed frame size
#define MAX_AMRNB_SIZE 33

// Maximum number of frames we are willing to decode in a packet
#define MAX_PKT_FRAMES  4

class AmrPlugin : public Plugin, public TranslatorFactory
{
public:
    AmrPlugin();
    ~AmrPlugin();
    virtual void initialize() {}
    virtual bool isBusy() const;
    virtual DataTranslator* create(const DataFormat& sFormat, const DataFormat& dFormat);
    virtual const TranslatorCaps* getCapabilities() const;
};

class AmrTrans : public DataTranslator
{
public:
    AmrTrans(const char* sFormat, const char* dFormat, void* amrState, bool octetAlign = false);
    virtual ~AmrTrans();
    virtual void Consume(const DataBlock& data, unsigned long tStamp);
    inline bool valid() const
	{ return 0 != m_amrState; }
    static inline const char* alignName(bool align)
	{ return align ? "octet aligned" : "bandwidth efficient"; }
protected:
    bool dataError(const char* text = 0);
    virtual bool pushData(unsigned long& tStamp) = 0;
    void* m_amrState;
    DataBlock m_data;
    bool m_showError;
    bool m_octetAlign;
    Mode m_cmr;
};

// Encoding specific class
class AmrEncoder : public AmrTrans
{
public:
    inline AmrEncoder(const char* sFormat, const char* dFormat, bool octetAlign, bool discont = false)
	: AmrTrans(sFormat,dFormat,::Encoder_Interface_init(discont ? 1 : 0),octetAlign),
	 m_mode(MR122)
	{ }
    virtual ~AmrEncoder();
protected:
    virtual bool pushData(unsigned long& tStamp);
    Mode m_mode;
};

// Decoding specific class
class AmrDecoder : public AmrTrans
{
public:
    inline AmrDecoder(const char* sFormat, const char* dFormat, bool octetAlign)
	: AmrTrans(sFormat,dFormat,::Decoder_Interface_init(),octetAlign)
	{ }
    virtual ~AmrDecoder();
protected:
    virtual bool pushData(unsigned long& tStamp);
};

// Module data
static int count = 0;                   // Created objects

static TranslatorCaps caps[] = {
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 }
};

// Voice bits per mode 0-7, 8 = Silence, 15 = No Data
static int modeBits[16] = {
    95, 103, 118, 134, 148, 159, 204, 244, 39,
    -1, -1, -1, -1, -1, -1, 0
};

// Discontinuous Transmission (DTX)
bool s_discontinuous = false;

// Helper function, gets a number of bits and advances pointer, return -1 for error
static int getBits(unsigned const char*& ptr, int& len, int& bpos, unsigned char bits)
{
    if (!ptr)
	return -1;
    if (!bits)
	return 0;
    int ret = 0;
    int mask = 0x80;
    while (bits--) {
	if (len <= 0)
	    return -1;
	if (((ptr[0] >> (7 - bpos)) & 1) != 0)
	    ret |= mask;
	mask = mask >> 1;
	if (++bpos >= 8) {
	    bpos = 0;
	    ptr++;
	    len--;
	}
    }
    return ret;
}


// Arbitrary type transcoder constructor
AmrTrans::AmrTrans(const char* sFormat, const char* dFormat, void* amrState, bool octetAlign)
    : DataTranslator(sFormat,dFormat),
      m_amrState(amrState), m_showError(true),
      m_octetAlign(octetAlign), m_cmr(MR122)
{
    Debug(MODNAME,DebugAll,"AmrTrans::AmrTrans('%s','%s',%p,%s) [%p]",
	sFormat,dFormat,amrState,String::boolText(octetAlign),this);
    count++;
}

// Destructor, closes the channel
AmrTrans::~AmrTrans()
{
    Debug(MODNAME,DebugAll,"AmrTrans::~AmrTrans() [%p]",this);
    m_amrState = 0;
    count--;
}

// Actual transcoding of data
void AmrTrans::Consume(const DataBlock& data, unsigned long tStamp)
{
    if (!(m_amrState && getTransSource()))
	return;
    ref();
    m_data += data;
    if (!tStamp)
	tStamp = timeStamp() + SAMPLES_FRAME;
    while (pushData(tStamp))
	;
    deref();
}

// Data error, report error 1st time and clear buffer
bool AmrTrans::dataError(const char* text)
{
    if (m_showError) {
	m_showError = false;
	const char* prefix = ": ";
	if (!text)
	    prefix = text = "";
	Debug(MODNAME,DebugWarn,"Error transcoding data%s%s [%p]",prefix,text,this);
    }
    m_data.clear();
    return false;
}


// Encoder cleanup
AmrEncoder::~AmrEncoder()
{
    Debug(MODNAME,DebugAll,"AmrEncoder::~AmrEncoder() %p [%p]",m_amrState,this);
    if (m_amrState)
	::Encoder_Interface_exit(m_amrState);
}

// Encode accumulated slin data and push it to the consumer
bool AmrEncoder::pushData(unsigned long& tStamp)
{
    if (m_data.length() < BUFFER_SIZE)
	return false;

    unsigned char unpacked[MAX_AMRNB_SIZE+1];
    int len = ::Encoder_Interface_Encode(m_amrState,m_mode,(short*)m_data.data(),unpacked,0);
    if ((len <= 0) || (len >= MAX_AMRNB_SIZE))
	return dataError("encoder");
    unpacked[len] = 0;
    XDebug(MODNAME,DebugAll,"Encoded mode %d frame to %d bytes first %02x [%p]",
	m_mode,len,unpacked[0],this);
    unsigned char buffer[MAX_AMRNB_SIZE];
    // build a TOC with just one entry
    if (m_octetAlign) {
	// 4 bit CMR, 4 bits reserved
	buffer[0] = (m_cmr << 4);
	// 1 bit follows (0), 4 bits of mode, 1 bit Q, 2 bits padding (0)
	buffer[1] = unpacked[0] & 0x7c;
	// AMR data
	for (int i = 1; i < len; i++)
	    buffer[i+1] = unpacked[i];
	len++;
    }
    else {
	// 4 bit CMR, 1 bit follows (forced 0), 3 bits of mode
	buffer[0] = (m_cmr << 4) | ((unpacked[0] >> 4) & 0x07);
	// 1 bit of mode and 1 bit Q
	unsigned char leftover = (unpacked[0] << 4) & 0xc0;
	// AMR data
	for (int i = 1; i < len; i++) {
	    buffer[i] = leftover | (unpacked[i] >> 2);
	    leftover = (unpacked[i] << 6) & 0xc0;
	}
    }
    m_data.cut(-BUFFER_SIZE);
    DataBlock outData(buffer,len,false);
    getTransSource()->Forward(outData,tStamp);
    outData.clear(false);
    tStamp += SAMPLES_FRAME;
    return (0 != m_data.length());
}


// Decoder cleanup
AmrDecoder::~AmrDecoder()
{
    Debug(MODNAME,DebugAll,"AmrDecoder::~AmrDecoder() %p [%p]",m_amrState,this);
    if (m_amrState)
	::Decoder_Interface_exit(m_amrState);
}

// Decode AMR data and push it to the consumer
bool AmrDecoder::pushData(unsigned long& tStamp)
{
    if (m_data.length() < 2)
	return false;
    unsigned const char* ptr = (unsigned const char*)m_data.data();
    int len = m_data.length();
    // an octet aligned packet should have 0 in the 4 reserved bits of CMR
    //  and in the lower 2 bits of first TOC entry octet
    bool octetHint = ((ptr[0] & 0x0f) | (ptr[1] & 0x03)) == 0;
    if (octetHint != m_octetAlign) {
	Debug(MODNAME,DebugNote,"Decoder switching from %s to %s mode [%p]",
	    alignName(m_octetAlign),alignName(octetHint),this);
	m_octetAlign = octetHint;
	// TODO: find and notify paired encoder about the new alignment
    }
    int bpos = 0;
    unsigned char cmr = getBits(ptr,len,bpos,4) >> 4;
    if (m_octetAlign)
	getBits(ptr,len,bpos,4);
    unsigned int tocLen = 0;
    unsigned char toc[MAX_PKT_FRAMES];
    int dataBits = 0;
    // read the TOC
    for (;;) {
	int ft = getBits(ptr,len,bpos,6);
	if (m_octetAlign)
	    getBits(ptr,len,bpos,2);
	if (ft < 0)
	    return dataError("TOC truncated");
	int nBits = modeBits[(ft >> 3) & 0x0f];
	// discard the entire packet if an invalid frame is found
	if (nBits < 0)
	    return dataError("invalid mode");
	if (m_octetAlign)
	    nBits = (nBits + 7) & (~7);
	dataBits += nBits;
	toc[tocLen++] = ft & 0x7c; // keep type and quality bit
	// does another TOC follow?
	if (0 == (ft & 0x80))
	    break;
	if (tocLen >= MAX_PKT_FRAMES)
	    return dataError("TOC too large");
    }
    if (dataBits > (8*len - bpos))
	return dataError("data truncated");
    // We read the TOC, now pick the following voice frames and decode
    for (unsigned int idx = 0; idx < tocLen; idx++) {
	if (m_octetAlign && (bpos != 0))
	    return dataError("internal alignment error");
	int mode = (toc[idx] >> 3) & 0x0f;
	bool good = 0 != (toc[idx] & 0x04);
	int nBits = modeBits[mode];
	XDebug(MODNAME,DebugAll,"Decoding %d bits %s mode %d frame %u [%p]",
	    nBits,(good ? "good" : "bad"),mode,idx,this);
	if (m_octetAlign)
	    nBits = (nBits + 7) & (~7);
	unsigned char unpacked[MAX_AMRNB_SIZE];
	unpacked[0] = toc[idx];
	for (unsigned int i = 1; i < MAX_AMRNB_SIZE; i++) {
	    int bits = (nBits <= 8) ? nBits : 8;
	    unpacked[i] = getBits(ptr,len,bpos,bits);
	    nBits -= bits;
	}
	short buffer[SAMPLES_FRAME];
	int type = (MRDTX == mode) ?
	    (good ? RxTypes::RX_SID_UPDATE : RxTypes::RX_SID_BAD) :
	    (good ? RxTypes::RX_SPEECH_GOOD : RxTypes::RX_SPEECH_DEGRADED);
	::Decoder_Interface_Decode(m_amrState,unpacked,buffer,type);
	DataBlock outData(buffer,BUFFER_SIZE,false);
	getTransSource()->Forward(outData,tStamp);
	outData.clear(false);
	tStamp += SAMPLES_FRAME;
    }
    if (bpos)
	len--;
    // now len holds how many bytes we should keep in data buffer
    m_data.cut(len-(int)m_data.length());
    if (cmr != m_cmr) {
	Debug(MODNAME,DebugNote,"Remote CMR changed from %d to %d [%p]",
	    m_cmr,cmr,this);
	m_cmr = (Mode)cmr;
	// TODO: find and notify paired encoder about the mode change request
    }
    return (0 != m_data.length());
}


// Plugin and translator factory
AmrPlugin::AmrPlugin()
{
    Output("Loaded module AMR-NB codec - based on 3GPP code");
    const FormatInfo* f = FormatRepository::addFormat("amr",0,20000);
    caps[0].src = caps[1].dest = f;
    f = FormatRepository::addFormat("amr-o",0,20000);
    caps[2].src = caps[3].dest = f;
    caps[0].dest = caps[1].src = caps[2].dest = caps[3].src = FormatRepository::getFormat("slin");
    // FIXME: put proper conversion costs
    caps[0].cost = caps[1].cost = caps[2].cost = caps[3].cost = 5;
}

AmrPlugin::~AmrPlugin()
{
    Output("Unloading module AMR-NB with %d codecs still in use",count);
}

bool AmrPlugin::isBusy() const
{
    return (count != 0);
}

// Create transcoder instance for requested formats
DataTranslator* AmrPlugin::create(const DataFormat& sFormat, const DataFormat& dFormat)
{
    if (sFormat == "slin") {
	if (dFormat == "amr")
	    return new AmrEncoder(sFormat,dFormat,false,s_discontinuous);
	if (dFormat == "amr-o")
	    return new AmrEncoder(sFormat,dFormat,true,s_discontinuous);
    }
    else if (dFormat == "slin") {
	if (sFormat == "amr")
	    return new AmrDecoder(sFormat,dFormat,false);
	if (sFormat == "amr-o")
	    return new AmrDecoder(sFormat,dFormat,true);
    }
    return 0;
}

const TranslatorCaps* AmrPlugin::getCapabilities() const
{
    return caps;
}


INIT_PLUGIN(AmrPlugin);

UNLOAD_PLUGIN(unloadNow)
{
    if (unloadNow)
	return !__plugin.isBusy();
    return true;
}

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */
