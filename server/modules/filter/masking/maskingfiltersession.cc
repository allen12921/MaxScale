/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "maskingfiltersession.hh"
#include <sstream>
#include <maxscale/buffer.hh>
#include <maxscale/filter.hh>
#include <maxscale/mysql_utils.h>
#include <maxscale/protocol/mysql.h>
#include "maskingfilter.hh"
#include "mysql.hh"

using maxscale::Buffer;
using std::ostream;
using std::string;
using std::stringstream;

MaskingFilterSession::MaskingFilterSession(SESSION* pSession, const MaskingFilter* pFilter)
    : maxscale::FilterSession(pSession)
    , m_filter(*pFilter)
    , m_state(IGNORING_RESPONSE)
{
}

MaskingFilterSession::~MaskingFilterSession()
{
}

//static
MaskingFilterSession* MaskingFilterSession::create(SESSION* pSession, const MaskingFilter* pFilter)
{
    return new MaskingFilterSession(pSession, pFilter);
}

int MaskingFilterSession::routeQuery(GWBUF* pPacket)
{
    ComRequest request(pPacket);

    // TODO: Breaks if responses are not waited for, before the next request is sent.
    switch (request.command())
    {
    case MYSQL_COM_QUERY:
    case MYSQL_COM_STMT_EXECUTE:
        m_res.reset(request.command(), m_filter.rules());
        m_state = EXPECTING_RESPONSE;
        break;

    default:
        m_state = IGNORING_RESPONSE;
    }

    return FilterSession::routeQuery(pPacket);
}

int MaskingFilterSession::clientReply(GWBUF* pPacket)
{
    MXS_NOTICE("clientReply");
    ss_dassert(GWBUF_IS_CONTIGUOUS(pPacket));

    switch (m_state)
    {
    case EXPECTING_NOTHING:
        MXS_WARNING("Received data, although expected nothing.");
    case IGNORING_RESPONSE:
        break;

    case EXPECTING_RESPONSE:
        handle_response(pPacket);
        break;

    case EXPECTING_FIELD:
        handle_field(pPacket);
        break;

    case EXPECTING_ROW:
        handle_row(pPacket);
        break;

    case EXPECTING_FIELD_EOF:
    case EXPECTING_ROW_EOF:
        handle_eof(pPacket);
        break;
    }

    return FilterSession::clientReply(pPacket);
}

void MaskingFilterSession::handle_response(GWBUF* pPacket)
{
    MXS_NOTICE("handle_response");
    ComResponse response(pPacket);

    switch (response.type())
    {
    case 0x00: // OK
    case 0xff: // ERR
    case 0xfb: // GET_MORE_CLIENT_DATA/SEND_MORE_CLIENT_DATA
        m_state = EXPECTING_NOTHING;
        break;

    default:
        {
            ComQueryResponse query_response(response);

            m_res.set_total_fields(query_response.nFields());
            m_state = EXPECTING_FIELD;
        }
    }
}

void MaskingFilterSession::handle_field(GWBUF* pPacket)
{
    MXS_NOTICE("handle_field");

    ComQueryResponse::ColumnDef column_def(pPacket);

    const char *zUser = session_get_user(m_pSession);
    const char *zHost = session_get_remote(m_pSession);

    if (!zUser)
    {
        zUser = "";
    }

    if (!zHost)
    {
        zHost = "";
    }

    const MaskingRules::Rule* pRule = m_res.rules()->get_rule_for(column_def, zUser, zHost);

    if (m_res.append_type_and_rule(column_def.type(), pRule))
    {
        // All fields have been read.
        m_state = EXPECTING_FIELD_EOF;
    }

    MXS_NOTICE("Stats: %s", column_def.to_string().c_str());
}

void MaskingFilterSession::handle_eof(GWBUF* pPacket)
{
    MXS_NOTICE("handle_eof");

    ComResponse response(pPacket);

    if (response.is_eof())
    {
        switch (m_state)
        {
        case EXPECTING_FIELD_EOF:
            m_state = EXPECTING_ROW;
            break;

        case EXPECTING_ROW_EOF:
            m_state = EXPECTING_NOTHING;
            break;

        default:
            ss_dassert(!true);
            m_state = IGNORING_RESPONSE;
        }
    }
    else
    {
        MXS_ERROR("Expected EOF, got something else: %d", response.type());
        m_state = IGNORING_RESPONSE;
    }
}

void MaskingFilterSession::handle_row(GWBUF* pPacket)
{
    MXS_NOTICE("handle_row");

    ComResponse response(pPacket);

    switch (response.type())
    {
    case ComPacket::EOF_PACKET:
        // EOF after last row.
        MXS_NOTICE("EOF after last row received.");
        m_state = EXPECTING_NOTHING;
        break;

    default:
        switch (m_res.command())
        {
        case MYSQL_COM_QUERY:
            {
                ComQueryResponse::Row row(response);

                ComQueryResponse::Row::iterator i = row.begin();
                while (i != row.end())
                {
                    const MaskingRules::Rule* pRule = m_res.get_rule();

                    if (pRule)
                    {
                        LEncString s = *i;

                        if (!s.is_null())
                        {
                            pRule->rewrite(s);
                        }

                        MXS_NOTICE("String: %s", (*i).to_string().c_str());
                    }
                    ++i;
                }
            }
            break;

        case MYSQL_COM_STMT_EXECUTE:
            {
                ComQueryResponse::BinaryRow row(response, m_res.types());

                ComQueryResponse::BinaryRow::iterator i = row.begin();
                while (i != row.end())
                {
                    const MaskingRules::Rule* pRule = m_res.get_rule();

                    if (pRule)
                    {
                        ComQueryResponse::BinaryRow::Value value = *i;

                        if (value.is_string())
                        {
                            LEncString s = value.as_string();
                            pRule->rewrite(s);
                        }
                    }
                    ++i;
                }
            }
            break;

        default:
            MXS_ERROR("Unexpected request: %d", m_res.command());
            ss_dassert(!true);
        }
    }
}
