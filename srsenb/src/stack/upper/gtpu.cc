/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */
#include "srslte/upper/gtpu.h"
#include "srsenb/hdr/stack/upper/gtpu.h"
#include "srslte/common/network_utils.h"
#include <errno.h>
#include <linux/ip.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace srslte;
namespace srsenb {

gtpu::gtpu(srslog::basic_logger& logger) : m1u(this), gtpu_log("GTPU"), logger(logger) {}

int gtpu::init(std::string                  gtp_bind_addr_,
               std::string                  mme_addr_,
               std::string                  m1u_multiaddr_,
               std::string                  m1u_if_addr_,
               srsenb::pdcp_interface_gtpu* pdcp_,
               stack_interface_gtpu_lte*    stack_,
               bool                         enable_mbsfn_)
{
  pdcp          = pdcp_;
  gtp_bind_addr = gtp_bind_addr_;
  mme_addr      = mme_addr_;
  pool          = byte_buffer_pool::get_instance();
  stack         = stack_;

  char errbuf[128] = {};

  // Set up socket
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    logger.error("Failed to create socket");
    return SRSLTE_ERROR;
  }
  int enable = 1;
#if defined(SO_REUSEADDR)
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    logger.error("setsockopt(SO_REUSEADDR) failed");
#endif
#if defined(SO_REUSEPORT)
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) < 0)
    logger.error("setsockopt(SO_REUSEPORT) failed");
#endif

  struct sockaddr_in bindaddr;
  bzero(&bindaddr, sizeof(struct sockaddr_in));
  bindaddr.sin_family      = AF_INET;
  bindaddr.sin_addr.s_addr = inet_addr(gtp_bind_addr.c_str());
  bindaddr.sin_port        = htons(GTPU_PORT);

  if (bind(fd, (struct sockaddr*)&bindaddr, sizeof(struct sockaddr_in))) {
    snprintf(errbuf, sizeof(errbuf), "%s", strerror(errno));
    logger.error("Failed to bind on address %s, port %d: %s", gtp_bind_addr.c_str(), int(GTPU_PORT), errbuf);
    srslte::console("Failed to bind on address %s, port %d: %s\n", gtp_bind_addr.c_str(), int(GTPU_PORT), errbuf);
    return SRSLTE_ERROR;
  }

  stack->add_gtpu_s1u_socket_handler(fd);

  // Start MCH socket if enabled
  enable_mbsfn = enable_mbsfn_;
  if (enable_mbsfn) {
    if (not m1u.init(m1u_multiaddr_, m1u_if_addr_)) {
      return SRSLTE_ERROR;
    }
  }
  return SRSLTE_SUCCESS;
}

void gtpu::stop()
{
  if (fd) {
    close(fd);
  }
}

// gtpu_interface_pdcp
void gtpu::write_pdu(uint16_t rnti, uint32_t lcid, srslte::unique_byte_buffer_t pdu)
{
  send_pdu_to_tunnel(rnti,
                     lcid,
                     std::move(pdu),
                     tunnels[ue_teidin_db[rnti][lcid][0]].teid_out,
                     tunnels[ue_teidin_db[rnti][lcid][0]].spgw_addr);
}
void gtpu::send_pdu_to_tunnel(uint16_t                     rnti,
                              uint32_t                     lcid,
                              srslte::unique_byte_buffer_t pdu,
                              uint32_t                     teidout,
                              uint32_t                     spgw_addr,
                              int                          pdcp_sn)
{
  logger.info(pdu->msg, pdu->N_bytes, "TX PDU, RNTI: 0x%x, LCID: %d, n_bytes=%d", rnti, lcid, pdu->N_bytes);

  // Check valid IP version
  struct iphdr* ip_pkt = (struct iphdr*)pdu->msg;
  if (ip_pkt->version != 4 && ip_pkt->version != 6) {
    logger.error("Invalid IP version to SPGW");
    return;
  }
  if (ip_pkt->version == 4) {
    if (ntohs(ip_pkt->tot_len) != pdu->N_bytes) {
      logger.error("IP Len and PDU N_bytes mismatch");
    }
    logger.debug("Tx S1-U PDU -- IP version %d, Total length %d", int(ip_pkt->version), ntohs(ip_pkt->tot_len));
    logger.debug("Tx S1-U PDU -- IP src addr %s", srslte::gtpu_ntoa(ip_pkt->saddr).c_str());
    logger.debug("Tx S1-U PDU -- IP dst addr %s", srslte::gtpu_ntoa(ip_pkt->daddr).c_str());
  }

  gtpu_header_t header;
  header.flags        = GTPU_FLAGS_VERSION_V1 | GTPU_FLAGS_GTP_PROTOCOL;
  header.message_type = GTPU_MSG_DATA_PDU;
  header.length       = pdu->N_bytes;
  header.teid         = teidout;

  if (pdcp_sn >= 0) {
    header.flags |= GTPU_FLAGS_EXTENDED_HDR;
    header.next_ext_hdr_type = GTPU_EXT_HEADER_PDCP_PDU_NUMBER;
    header.ext_buffer.resize(4u);
    header.ext_buffer[0] = 0x01u;
    header.ext_buffer[1] = (pdcp_sn >> 8u) & 0xffu;
    header.ext_buffer[2] = pdcp_sn & 0xffu;
    header.ext_buffer[3] = 0;
  }

  struct sockaddr_in servaddr;
  servaddr.sin_family      = AF_INET;
  servaddr.sin_addr.s_addr = htonl(spgw_addr);
  servaddr.sin_port        = htons(GTPU_PORT);

  if (!gtpu_write_header(&header, pdu.get(), gtpu_log)) {
    logger.error("Error writing GTP-U Header. Flags 0x%x, Message Type 0x%x", header.flags, header.message_type);
    return;
  }
  if (sendto(fd, pdu->msg, pdu->N_bytes, MSG_EOR, (struct sockaddr*)&servaddr, sizeof(struct sockaddr_in)) < 0) {
    perror("sendto");
  }
}

uint32_t gtpu::add_bearer(uint16_t rnti, uint32_t lcid, uint32_t addr, uint32_t teid_out, const bearer_props* props)
{
  // Allocate a TEID for the incoming tunnel
  uint32_t teid_in  = ++next_teid_in;
  tunnel&  new_tun  = tunnels[teid_in];
  new_tun.teid_in   = teid_in;
  new_tun.rnti      = rnti;
  new_tun.lcid      = lcid;
  new_tun.spgw_addr = addr;
  new_tun.teid_out  = teid_out;

  ue_teidin_db[rnti][lcid].push_back(teid_in);

  if (props != nullptr) {
    if (props->flush_before_teidin_present) {
      tunnel& after_tun               = tunnels.at(props->flush_before_teidin);
      after_tun.prior_teid_in_present = true;
      after_tun.prior_teid_in         = teid_in;
    }

    // Connect tunnels if forwarding is activated
    if (props->forward_from_teidin_present) {
      if (create_dl_fwd_tunnel(props->forward_from_teidin, teid_in) != SRSLTE_SUCCESS) {
        rem_tunnel(teid_in);
        return 0;
      }
    }
  }

  logger.info("Adding bearer for rnti: 0x%x, lcid: %d, addr: 0x%x, teid_out: 0x%x, teid_in: 0x%x",
              rnti,
              lcid,
              addr,
              teid_out,
              teid_in);

  return teid_in;
}

void gtpu::rem_bearer(uint16_t rnti, uint32_t lcid)
{
  auto ue_it = ue_teidin_db.find(rnti);
  if (ue_it == ue_teidin_db.end()) {
    logger.warning("Removing bearer rnti=0x%x, lcid=%d", rnti, lcid);
    return;
  }
  std::vector<uint32_t>& lcid_tuns = ue_it->second[lcid];

  while (not lcid_tuns.empty()) {
    rem_tunnel(lcid_tuns.back());
  }
  logger.info("Removing bearer for rnti: 0x%x, lcid: %d", rnti, lcid);

  bool rem_ue = std::all_of(
      ue_it->second.begin(), ue_it->second.end(), [](const std::vector<uint32_t>& list) { return list.empty(); });
  if (rem_ue) {
    ue_teidin_db.erase(ue_it);
  }
}

void gtpu::mod_bearer_rnti(uint16_t old_rnti, uint16_t new_rnti)
{
  logger.info("Modifying bearer rnti. Old rnti: 0x%x, new rnti: 0x%x", old_rnti, new_rnti);

  if (ue_teidin_db.count(new_rnti) != 0) {
    gtpu_log->error("New rnti already exists, aborting.\n");
    return;
  }
  auto old_it = ue_teidin_db.find(old_rnti);
  if (old_it == ue_teidin_db.end()) {
    gtpu_log->error("Old rnti does not exist, aborting.\n");
    return;
  }

  // Change RNTI bearers map
  ue_teidin_db.insert(std::make_pair(new_rnti, std::move(old_it->second)));
  ue_teidin_db.erase(old_it);

  // Change TEID
  auto new_it = ue_teidin_db.find(new_rnti);
  for (auto& bearer : new_it->second) {
    for (uint32_t teid : bearer) {
      tunnels[teid].rnti = new_rnti;
    }
  }
}

void gtpu::rem_tunnel(uint32_t teidin)
{
  auto it = tunnels.find(teidin);
  if (it == tunnels.end()) {
    logger.warning("Removing GTPU tunnel TEID In=0x%x", teidin);
    return;
  }
  if (it->second.fwd_teid_in_present) {
    // Forward End Marker to forwarding tunnel, before deleting tunnel
    end_marker(it->second.fwd_teid_in);
    it->second.fwd_teid_in_present = false;
  }
  auto                   ue_it        = ue_teidin_db.find(it->second.rnti);
  std::vector<uint32_t>& lcid_tunnels = ue_it->second[it->second.lcid];
  lcid_tunnels.erase(std::remove(lcid_tunnels.begin(), lcid_tunnels.end(), teidin), lcid_tunnels.end());
  tunnels.erase(it);
  logger.debug("TEID In=%d erased", teidin);
}

void gtpu::rem_user(uint16_t rnti)
{
  logger.info("Removing rnti=0x%x", rnti);
  auto ue_it = ue_teidin_db.find(rnti);
  if (ue_it != ue_teidin_db.end()) {
    for (auto& bearer : ue_it->second) {
      while (not bearer.empty()) {
        rem_tunnel(bearer.back());
      }
    }
  }
}

void gtpu::handle_gtpu_s1u_rx_packet(srslte::unique_byte_buffer_t pdu, const sockaddr_in& addr)
{
  logger.debug("Received %d bytes from S1-U interface", pdu->N_bytes);
  pdu->set_timestamp();

  gtpu_header_t header;
  if (not gtpu_read_header(pdu.get(), &header, gtpu_log)) {
    return;
  }

  if (header.teid != 0 && tunnels.count(header.teid) == 0) {
    // Received G-PDU for non-existing and non-zero TEID.
    // Sending GTP-U error indication
    error_indication(addr.sin_addr.s_addr, addr.sin_port, header.teid);
    return;
  }

  switch (header.message_type) {
    case GTPU_MSG_ECHO_REQUEST:
      // Echo request - send response
      echo_response(addr.sin_addr.s_addr, addr.sin_port, header.seq_number);
      break;
    case GTPU_MSG_DATA_PDU: {
      auto&    rx_tun = tunnels.find(header.teid)->second;
      uint16_t rnti   = rx_tun.rnti;
      uint16_t lcid   = rx_tun.lcid;

      if (lcid < SRSENB_N_SRB || lcid >= SRSENB_N_RADIO_BEARERS) {
        logger.error("Invalid LCID for DL PDU: %d - dropping packet", lcid);
        return;
      }

      struct iphdr* ip_pkt = (struct iphdr*)pdu->msg;
      if (ip_pkt->version != 4 && ip_pkt->version != 6) {
        logger.error("Invalid IP version to SPGW");
        return;
      }

      if (ip_pkt->version == 4) {
        if (ntohs(ip_pkt->tot_len) != pdu->N_bytes) {
          logger.error("IP Len and PDU N_bytes mismatch");
        }
        logger.debug("Rx S1-U PDU -- IPv%d, src=%s, dst=%s, length=%d",
                     int(ip_pkt->version),
                     srslte::gtpu_ntoa(ip_pkt->saddr).c_str(),
                     srslte::gtpu_ntoa(ip_pkt->daddr).c_str(),
                     ntohs(ip_pkt->tot_len));
      }

      if (rx_tun.fwd_teid_in_present) {
        tunnel& tx_tun = tunnels.at(rx_tun.fwd_teid_in);
        logger.info("Forwarding GTPU PDU rnti=0x%x, lcid=%d, n_bytes=%d", rnti, lcid, pdu->N_bytes);
        send_pdu_to_tunnel(rnti, lcid, std::move(pdu), tx_tun.teid_out, tx_tun.spgw_addr);
      } else if (rx_tun.prior_teid_in_present) {
        logger.info(
            pdu->msg, pdu->N_bytes, "Buffering RX GTPU PDU rnti=0x%x, lcid=%d, n_bytes=%d", rnti, lcid, pdu->N_bytes);
        rx_tun.buffer.push_back(std::move(pdu));
      } else {
        logger.info(pdu->msg, pdu->N_bytes, "RX GTPU PDU rnti=0x%x, lcid=%d, n_bytes=%d", rnti, lcid, pdu->N_bytes);
        uint32_t pdcp_sn = -1;
        if (header.flags & GTPU_FLAGS_EXTENDED_HDR and header.next_ext_hdr_type == GTPU_EXT_HEADER_PDCP_PDU_NUMBER) {
          pdcp_sn = (header.ext_buffer[1] << 8u) + header.ext_buffer[0];
        }
        pdcp->write_sdu(rnti, lcid, std::move(pdu), pdcp_sn);
      }
    } break;
    case GTPU_MSG_END_MARKER: {
      tunnel&  old_tun = tunnels.find(header.teid)->second;
      uint16_t rnti    = old_tun.rnti;
      logger.info("Received GTPU End Marker for rnti=0x%x.", rnti);

      // TS 36.300, Sec 10.1.2.2.1 - Path Switch upon handover
      if (old_tun.fwd_teid_in_present) {
        // END MARKER should be forwarded to TeNB if forwarding is activated
        end_marker(old_tun.fwd_teid_in);
        old_tun.fwd_teid_in_present = false;
      } else {
        // TeNB switches paths, and flush PDUs that have been buffered
        std::vector<uint32_t>& bearer_tunnels = ue_teidin_db.find(old_tun.rnti)->second[old_tun.lcid];
        for (uint32_t new_teidin : bearer_tunnels) {
          tunnel& new_tun = tunnels.at(new_teidin);
          if (new_teidin != old_tun.teid_in and new_tun.prior_teid_in_present and
              new_tun.prior_teid_in == old_tun.teid_in) {
            for (srslte::unique_byte_buffer_t& sdu : new_tun.buffer) {
              pdcp->write_sdu(new_tun.rnti, new_tun.lcid, std::move(sdu));
            }
            new_tun.prior_teid_in_present = false;
            new_tun.buffer.clear();
          }
        }
      }
      break;
    }
    default:
      break;
  }
}

void gtpu::handle_gtpu_m1u_rx_packet(srslte::unique_byte_buffer_t pdu, const sockaddr_in& addr)
{
  m1u.handle_rx_packet(std::move(pdu), addr);
}

/// Connect created tunnel with pre-existing tunnel for data forwarding
int gtpu::create_dl_fwd_tunnel(uint32_t rx_teid_in, uint32_t tx_teid_in)
{
  auto rx_tun_pair = tunnels.find(rx_teid_in);
  auto tx_tun_pair = tunnels.find(tx_teid_in);
  if (rx_tun_pair == tunnels.end() or tx_tun_pair == tunnels.end()) {
    logger.error("Failed to create forwarding tunnel between teids 0x%x and 0x%x", rx_teid_in, tx_teid_in);
    return SRSLTE_ERROR;
  }

  tunnel &rx_tun = rx_tun_pair->second, &tx_tun = tx_tun_pair->second;
  rx_tun.fwd_teid_in_present = true;
  rx_tun.fwd_teid_in         = tx_teid_in;
  logger.info("Creating forwarding tunnel for rnti=0x%x, lcid=%d, in={0x%x, 0x%x}->out={0x%x, 0x%x}",
              rx_tun.rnti,
              rx_tun.lcid,
              rx_tun.teid_out,
              rx_tun.spgw_addr,
              tx_tun.teid_out,
              tx_tun.spgw_addr);

  // Get all buffered PDCP PDUs, and forward them through tx tunnel
  std::map<uint32_t, srslte::unique_byte_buffer_t> pdus = pdcp->get_buffered_pdus(rx_tun.rnti, rx_tun.lcid);
  for (auto& pdu_pair : pdus) {
    send_pdu_to_tunnel(
        rx_tun.rnti, rx_tun.lcid, std::move(pdu_pair.second), tx_tun.teid_out, tx_tun.spgw_addr, pdu_pair.first);
  }

  return SRSLTE_SUCCESS;
}

/****************************************************************************
 * GTP-U Error Indication
 ***************************************************************************/
void gtpu::error_indication(in_addr_t addr, in_port_t port, uint32_t err_teid)
{
  logger.info("TX GTPU Error Indication. Seq: %d, Error TEID: %d", tx_seq, err_teid);

  gtpu_header_t        header = {};
  unique_byte_buffer_t pdu    = allocate_unique_buffer(*pool);

  // header
  header.flags             = GTPU_FLAGS_VERSION_V1 | GTPU_FLAGS_GTP_PROTOCOL | GTPU_FLAGS_SEQUENCE;
  header.message_type      = GTPU_MSG_ERROR_INDICATION;
  header.teid              = err_teid;
  header.length            = 4;
  header.seq_number        = tx_seq;
  header.n_pdu             = 0;
  header.next_ext_hdr_type = 0;

  gtpu_write_header(&header, pdu.get(), gtpu_log);

  struct sockaddr_in servaddr;
  servaddr.sin_family      = AF_INET;
  servaddr.sin_addr.s_addr = addr;
  servaddr.sin_port        = port;

  sendto(fd, pdu->msg, 12, MSG_EOR, (struct sockaddr*)&servaddr, sizeof(struct sockaddr_in));
  tx_seq++;
}

/****************************************************************************
 * GTP-U Echo Request/Response
 ***************************************************************************/
void gtpu::echo_response(in_addr_t addr, in_port_t port, uint16_t seq)
{
  logger.info("TX GTPU Echo Response, Seq: %d", seq);

  gtpu_header_t        header = {};
  unique_byte_buffer_t pdu    = allocate_unique_buffer(*pool);

  // header
  header.flags             = GTPU_FLAGS_VERSION_V1 | GTPU_FLAGS_GTP_PROTOCOL | GTPU_FLAGS_SEQUENCE;
  header.message_type      = GTPU_MSG_ECHO_RESPONSE;
  header.teid              = 0;
  header.length            = 4;
  header.seq_number        = seq;
  header.n_pdu             = 0;
  header.next_ext_hdr_type = 0;

  gtpu_write_header(&header, pdu.get(), gtpu_log);

  struct sockaddr_in servaddr;
  servaddr.sin_family      = AF_INET;
  servaddr.sin_addr.s_addr = addr;
  servaddr.sin_port        = port;

  sendto(fd, pdu->msg, 12, MSG_EOR, (struct sockaddr*)&servaddr, sizeof(struct sockaddr_in));
}

/****************************************************************************
 * GTP-U END MARKER
 ***************************************************************************/
void gtpu::end_marker(uint32_t teidin)
{
  logger.info("TX GTPU End Marker.");
  tunnel& tunnel = tunnels.find(teidin)->second;

  gtpu_header_t        header = {};
  unique_byte_buffer_t pdu    = allocate_unique_buffer(*pool);

  // header
  header.flags        = GTPU_FLAGS_VERSION_V1 | GTPU_FLAGS_GTP_PROTOCOL;
  header.message_type = GTPU_MSG_END_MARKER;
  header.teid         = tunnel.teid_out;
  header.length       = 0;

  gtpu_write_header(&header, pdu.get(), gtpu_log);

  struct sockaddr_in servaddr;
  servaddr.sin_family      = AF_INET;
  servaddr.sin_addr.s_addr = htonl(tunnel.spgw_addr);
  servaddr.sin_port        = htons(GTPU_PORT);

  sendto(fd, pdu->msg, 12, MSG_EOR, (struct sockaddr*)&servaddr, sizeof(struct sockaddr_in));
}

/****************************************************************************
 * TEID to RNTI/LCID helper functions
 ***************************************************************************/

gtpu::tunnel* gtpu::get_tunnel(uint32_t teidin)
{
  auto it = tunnels.find(teidin);
  if (it == tunnels.end()) {
    logger.error("TEID=%d In does not exist.", teidin);
    return nullptr;
  }
  return &it->second;
}

/****************************************************************************
 * Class to handle MCH packet handling
 ***************************************************************************/

gtpu::m1u_handler::~m1u_handler()
{
  if (initiated) {
    close(m1u_sd);
    initiated = false;
  }
}

bool gtpu::m1u_handler::init(std::string m1u_multiaddr_, std::string m1u_if_addr_)
{
  m1u_multiaddr = std::move(m1u_multiaddr_);
  m1u_if_addr   = std::move(m1u_if_addr_);
  pdcp          = parent->pdcp;
  gtpu_log      = parent->gtpu_log;

  // Set up sink socket
  struct sockaddr_in bindaddr = {};
  m1u_sd                      = socket(AF_INET, SOCK_DGRAM, 0);
  if (m1u_sd < 0) {
    logger.error("Failed to create M1-U sink socket");
    return false;
  }

  /* Bind socket */
  bindaddr.sin_family      = AF_INET;
  bindaddr.sin_addr.s_addr = htonl(INADDR_ANY); // Multicast sockets require bind to INADDR_ANY
  bindaddr.sin_port        = htons(GTPU_PORT + 1);
  if (bind(m1u_sd, (struct sockaddr*)&bindaddr, sizeof(bindaddr)) < 0) {
    logger.error("Failed to bind multicast socket");
    return false;
  }

  /* Send an ADD MEMBERSHIP message via setsockopt */
  struct ip_mreq mreq {};
  mreq.imr_multiaddr.s_addr = inet_addr(m1u_multiaddr.c_str()); // Multicast address of the service
  mreq.imr_interface.s_addr = inet_addr(m1u_if_addr.c_str());   // Address of the IF the socket will listen to.
  if (setsockopt(m1u_sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    logger.error("Register musticast group for M1-U");
    logger.error("M1-U infterface IP: %s, M1-U Multicast Address %s", m1u_if_addr.c_str(), m1u_multiaddr.c_str());
    return false;
  }
  logger.info("M1-U initialized");

  initiated    = true;
  lcid_counter = 1;

  // Register socket in stack rx sockets thread
  parent->stack->add_gtpu_m1u_socket_handler(m1u_sd);

  return true;
}

void gtpu::m1u_handler::handle_rx_packet(srslte::unique_byte_buffer_t pdu, const sockaddr_in& addr)
{
  logger.debug("Received %d bytes from M1-U interface", pdu->N_bytes);

  gtpu_header_t header;
  gtpu_read_header(pdu.get(), &header, gtpu_log);
  pdcp->write_sdu(SRSLTE_MRNTI, lcid_counter, std::move(pdu));
}

} // namespace srsenb
