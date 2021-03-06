/*
 * MoldUDP protocol support
 *
 * The implementation is based on the following specifications provided
 * by NASDAQ OMX:
 *
 *   MoldUDP
 *   Version 1.02a
 *   October 19, 2006
 *
 * and
 *
 *   MoldUDP for NASDAQ OMX Nordic
 *   Version 1.0.1
 *   February 10, 2014
 */

#pragma once

#include "helix/net.hh"

#include <cstdint>
#include <memory>

namespace helix {

namespace nasdaq {

class moldudp_session : public net::message_parser {
private:
    std::shared_ptr<net::message_parser> _parser;
    uint32_t _seq_num;
public:
    explicit moldudp_session(std::shared_ptr<net::message_parser> parser);

    virtual size_t parse(const net::packet_view& packet) override;
};

}

}
