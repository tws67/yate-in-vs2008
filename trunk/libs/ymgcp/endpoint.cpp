/**
 * endpoint.cpp
 * Yet Another MGCP Stack
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

#include <yatemgcp.h>

using namespace TelEngine;

/**
 * MGCPEndpoint
 */
// Construct the id. Append itself to the engine's list
MGCPEndpoint::MGCPEndpoint(MGCPEngine* engine, const char* user,
	const char* host, int port)
    : MGCPEndpointId(user,host,port),
    m_engine(engine)
{
    if (!m_engine) {
	Debug(DebugNote,"Can't construct endpoint without engine [%p]",this);
	return;
    }
    m_engine->attach(this);
}

// Remove itself from engine's list
MGCPEndpoint::~MGCPEndpoint()
{
    if (m_engine)
	m_engine->detach(this);
}

// Append info about a remote endpoint controlled by or controlling this endpoint.
// If the engine owning this endpoint is an MGCP gateway, only 1 remote peer (Call Agent) is allowed
MGCPEpInfo* MGCPEndpoint::append(const char* endpoint, const char* host, int port)
{
    if (!m_engine || (m_engine->gateway() && m_remote.count() >= 1))
	return 0;

    if (!endpoint)
	endpoint = user();
    if (!port)
	port = m_engine->defaultPort(!m_engine->gateway());
    MGCPEpInfo* ep = new MGCPEpInfo(endpoint,host,port);
    if (!ep->valid() || find(ep->id()))
	TelEngine::destruct(ep);
    else
	m_remote.append(ep);
    return ep;
}

//  Find the info object associated with a remote peer
MGCPEpInfo* MGCPEndpoint::find(const char* epId)
{
    Lock lock(m_mutex);
    ObjList* obj = m_remote.find(epId);
    return obj ? static_cast<MGCPEpInfo*>(obj->get()) : 0;
}

// Find the info object associated with an unique remote peer
MGCPEpInfo* MGCPEndpoint::peer()
{
    return (m_remote.count() == 1) ? static_cast<MGCPEpInfo*>(m_remote.get()) : 0;
}

/**
 * MGCPEndpointId
 */
// Set this endpoint id. Convert it to lower case
void MGCPEndpointId::set(const char* endpoint, const char* host, int port)
{
    m_id = "";
    m_endpoint = endpoint;
    m_endpoint.toLower();
    m_host = host;
    m_host.toLower();
    m_port = port;
    m_id << m_endpoint << "@" << m_host;
    if (m_port)
	m_id << ":" << m_port;
}

/* vi: set ts=8 sw=4 sts=4 noet: */