#ifndef _NETP_SOCKET_HPP_
#define _NETP_SOCKET_HPP_

#include <queue>

#include <netp/smart_ptr.hpp>
#include <netp/string.hpp>
#include <netp/packet.hpp>
#include <netp/address.hpp>

#include <netp/socket_base.hpp>
#include <netp/channel.hpp>
#include <netp/dns_resolver.hpp>

#if defined(_NETP_WIN) && defined(NETP_ENABLE_IOCP)
	#define NETP_DEFAULT_LISTEN_BACKLOG SOMAXCONN
//	#define NETP_DEFAULT_LISTEN_BACKLOG 512
#else
	#define NETP_DEFAULT_LISTEN_BACKLOG 256
#endif

#define NETP_ENABLE_FAST_WRITE

//in milliseconds
#define NETP_SOCKET_BDLIMIT_TIMER_DELAY_DUR (250)

namespace netp {

	struct socket_url_parse_info {
		string_t proto;
		string_t host;
		port_t port;
	};

	static int parse_socket_url(const char* url, size_t len, socket_url_parse_info& info) {
		std::vector<string_t> _arr;
		netp::split<string_t>(string_t(url, len), ":", _arr);
		if (_arr.size() != 3) {
			return netp::E_SOCKET_INVALID_ADDRESS;
		}

		info.proto = _arr[0];
		if (_arr[1].substr(0, 2) != "//") {
			return netp::E_SOCKET_INVALID_ADDRESS;
		}
		
		info.host = _arr[1].substr(2);
		info.port = netp::to_u32(_arr[2].c_str())&0xFFFF;

		return netp::OK;
	}

	static inline std::tuple<int, u8_t, u8_t, u16_t> __inspect_address_info_from_dial_str(const char* dialstr ) {
		u16_t sproto = DEF_protocol_str_to_proto(dialstr);
		u8_t family;
		u8_t stype;
		switch (sproto) {
			case (NETP_PROTOCOL_TCP) :
			{
				family = NETP_AF_INET;
				stype = NETP_SOCK_STREAM;
			}
			break;
			case (NETP_PROTOCOL_UDP) :
			{
				family = NETP_AF_INET;
				stype = NETP_SOCK_DGRAM;
			}
			break;
			default:
			{
				family = (NETP_AF_USER);
				stype = NETP_SOCK_USERPACKET;
			}
		}

		return std::make_tuple(netp::OK, family,stype, sproto);
	}

	class socket_cfg final :
		public ref_base
	{
	public:
		NRP<io_event_loop> L;
		SOCKET fd;
		u8_t family;
		u8_t type;
		u16_t proto;
		u16_t option;

		address laddr;
		address raddr;

		socket_api* sockapi;
		keep_alive_vals kvals;
		channel_buf_cfg sock_buf;
		u32_t bdlimit; //in bit (1kb == 1024b), 0 means no limit
		
		socket_cfg( NRP<io_event_loop> const& L = nullptr ):
			L(L),
			fd(SOCKET(NETP_INVALID_SOCKET)),
			family((NETP_AF_INET)),
			type(NETP_SOCK_STREAM),
			proto(NETP_PROTOCOL_TCP),
			option(default_socket_option),
			laddr(),
			raddr(),
			sockapi((netp::socket_api*)&netp::NETP_DEFAULT_SOCKAPI),
			kvals(default_tcp_keep_alive_vals),
			sock_buf({0}),
			bdlimit(0)
		{}
	};

	struct socket_outbound_entry final {
		NRP<packet> data;
		NRP<promise<int>> write_promise;
		address to;
	};

	class socket final :
		public channel,
		public socket_base
	{
		byte_t* m_rcv_buf_ptr;
		u32_t m_rcv_buf_size;
#ifdef NETP_IO_MODE_IOCP
		WSAOVERLAPPED* m_ol_write;
#endif

		typedef std::deque<socket_outbound_entry, netp::allocator<socket_outbound_entry>> socket_outbound_entry_t;
		socket_outbound_entry_t m_outbound_entry_q;
		netp::size_t m_noutbound_bytes;


		netp::size_t m_outbound_budget;
		netp::size_t m_outbound_limit; //in byte

		void _tmcb_BDL(NRP<timer> const& t);
	public:
		socket( NRP<socket_cfg> const& cfg):
			channel(cfg->L),
			socket_base(cfg->fd, cfg->family, cfg->type, cfg->proto, cfg->laddr, cfg->raddr, cfg->sockapi),
			m_rcv_buf_ptr(cfg->L->channel_rcv_buf()->head()),
			m_rcv_buf_size(u32_t(cfg->L->channel_rcv_buf()->left_right_capacity())),
#ifdef NETP_IO_MODE_IOCP
			m_ol_write(0),
#endif
			m_noutbound_bytes(0),
			m_outbound_budget(cfg->bdlimit),
			m_outbound_limit(cfg->bdlimit)
		{
			NETP_ASSERT(cfg->L != nullptr);
		}

		~socket()
		{
		}

	public:
		int bind(address const& addr);
		int listen( int backlog = NETP_DEFAULT_LISTEN_BACKLOG);

		SOCKET accept(address& raddr);
		int connect(address const& addr);
		void do_async_connect(address const& addr, NRP<promise<int>> const& p);

		static std::tuple<int, NRP<socket>> create(NRP<socket_cfg> const& cfg) {
			NETP_ASSERT(cfg->L != nullptr);
			NETP_ASSERT(cfg->L->in_event_loop());
			NETP_ASSERT(cfg->proto == NETP_PROTOCOL_USER ? cfg->L->type() != NETP_DEFAULT_POLLER_TYPE: true );

			NRP<socket> so = netp::make_ref<socket>(cfg);

			int rt;
			if (cfg->fd == NETP_INVALID_SOCKET) {
				rt = so->open();
				if (rt != netp::OK) {
					NETP_WARN("[socket][%s]open failed: %d", so->info().c_str(), rt);
					return std::make_tuple(rt, nullptr);
				}
			}

			rt = so->init(cfg->option, cfg->kvals, cfg->sock_buf);
			if (rt != netp::OK) {
				so->close();
				NETP_WARN("[socket][%s]init failed: %d", so->info().c_str(), rt);
				return std::make_tuple(rt, nullptr);
			}

			so->ch_init();
			return std::make_tuple(rt, so);
		}

		static void do_async_create(NRP<netp::socket_cfg> const& cfg, NRP <netp::promise<std::tuple<int, NRP<socket>>>> const& p) {
			if (cfg->L == nullptr) {
				cfg->L = netp::io_event_loop_group::instance()->next(NETP_DEFAULT_POLLER_TYPE);
			}
			cfg->L->execute([cfg, p]() {
				p->set(socket::create(cfg));
			});
		}

		static NRP<netp::promise<std::tuple<int, NRP<socket>>>> async_create(NRP<netp::socket_cfg> const& cfg) {
			NRP <netp::promise<std::tuple<int, NRP<socket>>>> p = netp::make_ref<netp::promise<std::tuple<int, NRP<socket>>>>();
			if (cfg->L == nullptr) {
				cfg->L = netp::io_event_loop_group::instance()->next(NETP_DEFAULT_POLLER_TYPE);
			}
			socket::do_async_create(cfg, p);
			return p;
		}

		static void do_dial( address const& addr, fn_channel_initializer_t const& initializer, NRP<channel_dial_promise> const& ch_dialf, NRP<socket_cfg> const& cfg ) {
			if (cfg->L == nullptr) {
				NETP_ASSERT(cfg->type != NETP_AF_USER );
				cfg->L = io_event_loop_group::instance()->next();
			}
			if (!cfg->L->in_event_loop()) {
				cfg->L->schedule([addr,initializer,ch_dialf, cfg]() {
					socket::do_dial(addr,initializer,ch_dialf,cfg);
				});
				return;
			}

			std::tuple<int, NRP<socket>> tupc = create(cfg);
			int rt = std::get<0>(tupc);
			if (rt != netp::OK) {
				ch_dialf->set(std::make_tuple(rt, nullptr));
				return;
			}

			NRP<promise<int>> so_dialf = netp::make_ref<promise<int>>();
			NRP<socket> so = std::get<1>(tupc);
			so_dialf->if_done([ch_dialf, so](int const& rt) {
				if (rt == netp::OK) {
					ch_dialf->set(std::make_tuple(rt, so));
				} else {
					ch_dialf->set(std::make_tuple(rt, nullptr));
					so->ch_errno() = rt;
					so->ch_flag() |= int(channel_flag::F_WRITE_ERROR);
					so->ch_close_impl(nullptr);
				}
			});

			so->do_dial(addr, initializer, so_dialf);
		}

		static void do_dial( netp::size_t idx, std::vector<address> const& addrs, fn_channel_initializer_t const& initializer, NRP<channel_dial_promise> const& ch_dialf, NRP<socket_cfg> const& cfg ) {
			if (idx >= addrs.size() ) {
				NETP_WARN("[socket]dail failed after try count: %u", idx );
				ch_dialf->set(std::make_tuple(netp::E_SOCKET_NO_AVAILABLE_ADDR, nullptr));
				return;
			}

			NRP<channel_dial_promise> _dp = netp::make_ref<channel_dial_promise>();
			_dp->if_done([nidx=(idx+1),addrs,initializer, ch_dialf, cfg ](std::tuple<int,NRP<channel>> const& tupc) {
				int dialrt = std::get<0>(tupc);
				if (dialrt == netp::OK) {
					ch_dialf->set(tupc);
					return;
				}

				socket::do_dial(nidx, addrs,initializer, ch_dialf, cfg );
			});

			socket::do_dial(addrs[idx], initializer, _dp, cfg);
		}

	public:
		static void do_dial(const char* dialurl, size_t len, fn_channel_initializer_t const& initializer, NRP<channel_dial_promise> const& ch_dialf, NRP<socket_cfg> const& cfg ) {
			socket_url_parse_info info;
			int rt = parse_socket_url(dialurl, len, info);
			if (rt != netp::OK) {
				ch_dialf->set(std::make_tuple( rt, nullptr));
				return;
			}

			std::tie(rt, cfg->family, cfg->type, cfg->proto) = __inspect_address_info_from_dial_str(info.proto.c_str());
			if (rt != netp::OK) {
				ch_dialf->set( std::make_tuple(rt,nullptr));
				return;
			}

			if (netp::is_dotipv4_decimal_notation(info.host.c_str())) {
				do_dial(address(info.host.c_str(), info.port, cfg->family), initializer, ch_dialf, cfg);
				return;
			}

			NRP<dns_query_promise> dnsp = netp::dns_resolver::instance()->resolve(info.host);
			dnsp->if_done([port = info.port, initializer, ch_dialf, cfg](std::tuple<int, std::vector<ipv4_t,netp::allocator<ipv4_t>>> const& tupdns ) {
				if ( std::get<0>(tupdns) != netp::OK) {
					ch_dialf->set(std::make_tuple(std::get<0>(tupdns), nullptr));
					return;
				}

				std::vector<ipv4_t,netp::allocator<ipv4_t>> const& ipv4s = std::get<1>(tupdns);
				NETP_ASSERT(ipv4s.size());
				std::vector<address> dialaddrs;
				for (netp::size_t i = 0; i <ipv4s.size(); ++i) {
					address __a;
					__a.setipv4(ipv4s[i]);
					__a.setport(port);
					__a.setfamily(cfg->family);
					dialaddrs.push_back( __a );
				}

				socket::do_dial(0, dialaddrs, initializer, ch_dialf, cfg);
			});
		}

		static void do_dial(std::string const& dialurl, fn_channel_initializer_t const& initializer, NRP<channel_dial_promise> const& ch_dialf, NRP<socket_cfg> const& ccfg) {
			socket::do_dial(dialurl.c_str(), dialurl.length(), initializer, ch_dialf, ccfg);
		}

		static NRP<channel_dial_promise> dial(const char* dialurl, size_t len, fn_channel_initializer_t const& initializer, NRP<socket_cfg> const& ccfg ) {
			NRP<channel_dial_promise> f = netp::make_ref<channel_dial_promise>();
			socket::do_dial(dialurl, len, initializer, f, ccfg );
			return f;
		}

		static NRP<channel_dial_promise> dial(std::string const& dialurl, fn_channel_initializer_t const& initializer, NRP<socket_cfg> const& ccfg) {
			NRP<channel_dial_promise> f = netp::make_ref<channel_dial_promise>();
			socket::do_dial(dialurl.c_str(), dialurl.length(), initializer, f, ccfg);
			return f;
		}

		static NRP<channel_dial_promise> dial(std::string const& dialurl, fn_channel_initializer_t const& initializer) {
			NRP<channel_dial_promise> f = netp::make_ref<channel_dial_promise>();
			socket::do_dial(dialurl.c_str(), dialurl.length(), initializer, f, netp::make_ref<socket_cfg>());
			return f;
		}

		/*
		 example:
			socket::listen("tcp://127.0.0.1:80", [](NRP<channel> const& ch) {
				NRP<netp::channel_handler_abstract> h = netp::make_ref<netp::handler::echo>():
				ch->pipeline()->add_last(h);
			});
		*/

		static void do_listen_on(NRP<channel_listen_promise> const& listenp, address const& laddr, fn_channel_initializer_t const& initializer, NRP<socket_cfg> const& cfg, int backlog) {
			NETP_ASSERT(cfg->L != nullptr);
			NETP_ASSERT(cfg->L->in_event_loop());

			cfg->option &= ~int(socket_option::OPTION_KEEP_ALIVE);
			cfg->option &= ~int(socket_option::OPTION_NODELAY);

			std::tuple<int, NRP<socket>> tupc = create(cfg);
			int rt = std::get<0>(tupc);
			if (rt != netp::OK) {
				NETP_WARN("[socket]do_listen_on failed: %d, listen addr: %s", rt, laddr.to_string().c_str() );
				listenp->set(std::make_tuple(rt, nullptr));
				return ;
			}

			NRP<socket> so = std::get<1>(tupc);
			NRP<promise<int>> listen_f = netp::make_ref<promise<int>>();
			listen_f->if_done([listenp, so](int const& rt) {
				if (rt == netp::OK) {
					listenp->set(std::make_tuple(netp::OK, so));
				} else {
					listenp->set(std::make_tuple(rt, nullptr));
					so->ch_errno() = rt;
					so->m_chflag |= int(channel_flag::F_READ_ERROR);//for assert check
					so->ch_close_impl(nullptr);
				}
			});
			so->do_listen_on(laddr, initializer, listen_f, cfg, backlog);
		}

		static NRP<channel_listen_promise> listen_on(const char* listenurl, size_t len, fn_channel_initializer_t const& initializer, NRP<socket_cfg> const& cfg, int backlog = NETP_DEFAULT_LISTEN_BACKLOG) {
			NRP<channel_listen_promise> listenp = netp::make_ref<channel_listen_promise>();

			socket_url_parse_info info;
			int rt = parse_socket_url(listenurl, len,info);
			if (rt != netp::OK) {
				listenp->set(std::make_tuple(rt, nullptr));
				return listenp;
			}

			std::tie(rt, cfg->family, cfg->type, cfg->proto) = __inspect_address_info_from_dial_str(info.proto.c_str());
			if (rt != netp::OK) {
				listenp->set(std::make_tuple(rt, nullptr));
				return listenp;
			}

			if (!netp::is_dotipv4_decimal_notation(info.host.c_str())) {
				listenp->set(std::make_tuple(netp::E_SOCKET_INVALID_ADDRESS, nullptr));
				return listenp;
			}

			address laddr(info.host.c_str(), info.port, cfg->family );
			if (cfg->L == nullptr) {
				cfg->L = io_event_loop_group::instance()->next(NETP_DEFAULT_POLLER_TYPE);
			}

			if (!cfg->L->in_event_loop()) {
				cfg->L->schedule([listenp, laddr, initializer, cfg, backlog]() {
					socket::do_listen_on(listenp, laddr, initializer, cfg, backlog);
				});
				return listenp;
			}

			socket::do_listen_on(listenp, laddr, initializer, cfg, backlog);
			return listenp;
		}

		static NRP<channel_listen_promise> listen_on(std::string const& listenurl, fn_channel_initializer_t const& initializer, NRP<socket_cfg> const& cfg, int backlog = NETP_DEFAULT_LISTEN_BACKLOG) {
			return socket::listen_on(listenurl.c_str(), listenurl.length(), initializer,cfg,backlog);
		}

		static NRP<channel_listen_promise> listen_on(const char* listenurl, size_t len, fn_channel_initializer_t const& initializer) {
			return socket::listen_on(listenurl, len, initializer, netp::make_ref<socket_cfg>(), NETP_DEFAULT_LISTEN_BACKLOG);
		}

		static NRP<channel_listen_promise> listen_on(std::string const& listenurl, fn_channel_initializer_t const& initializer) {
			return socket::listen_on(listenurl.c_str(), listenurl.length() , initializer, netp::make_ref<socket_cfg>(), NETP_DEFAULT_LISTEN_BACKLOG);
		}

	private:

		//url example: tcp://0.0.0.0:80, udp://127.0.0.1:80		
		//@todo
		//tcp6://ipv6address
		void do_listen_on(address const& addr, fn_channel_initializer_t const& fn_accepted, NRP<promise<int>> const& chp, NRP<socket_cfg> const& ccfg, int backlog = NETP_DEFAULT_LISTEN_BACKLOG);
		//NRP<promise<int>> listen_on(address const& addr, fn_channel_initializer_t const& fn_accepted, NRP<socket_cfg> const& cfg, int backlog = NETP_DEFAULT_LISTEN_BACKLOG);

		void do_dial(address const& addr, fn_channel_initializer_t const& initializer, NRP<promise<int>> const& chp);
		//NRP<promise<int>> dial(address const& addr, fn_channel_initializer_t const& initializer);

		void _ch_do_close_read() {
			if ((m_chflag&int(channel_flag::F_READ_SHUTDOWN))) { return; }

			NETP_ASSERT((m_chflag & int(channel_flag::F_READ_SHUTDOWNING)) == 0);
			m_chflag |= int(channel_flag::F_READ_SHUTDOWNING);

			socket::ch_aio_end_read();
			//end_read and log might result in F_READ_SHUTDOWN state. (FOR net_logger)

			socket_base::shutdown(SHUT_RD);
			m_chflag |= int(channel_flag::F_READ_SHUTDOWN);
			m_chflag &= ~int(channel_flag::F_READ_SHUTDOWNING);
			ch_fire_read_closed();
			NETP_TRACE_SOCKET("[socket][%s]ch_do_close_read end, errno: %d, flag: %d", info().c_str(), ch_errno(), m_chflag);
			ch_rdwr_shutdown_check();
		}

		void _ch_do_close_write() {
			if (m_chflag&int(channel_flag::F_WRITE_SHUTDOWN)) { return; }

			//boundary checking&set
			NETP_ASSERT( (m_chflag & int(channel_flag::F_WRITE_SHUTDOWNING)) ==0 );
			m_chflag |= int(channel_flag::F_WRITE_SHUTDOWNING);

			m_chflag &= ~int(channel_flag::F_WRITE_SHUTDOWN_PENDING);
			ch_aio_end_write();

			while ( m_outbound_entry_q.size() ) {
				NETP_ASSERT((ch_errno() != 0) && (m_chflag & (int(channel_flag::F_WRITE_ERROR) | int(channel_flag::F_READ_ERROR) | int(channel_flag::F_IO_EVENT_LOOP_NOTIFY_TERMINATING))));
				socket_outbound_entry& entry = m_outbound_entry_q.front();
				NETP_WARN("[socket][%s]cancel outbound, nbytes:%u, errno: %d", info().c_str(), entry.data->len(), ch_errno() );
				//hold a copy before we do pop it from queue
				NRP<promise<int>> wp = entry.write_promise;
				m_noutbound_bytes -= entry.data->len();
				m_outbound_entry_q.pop_front();
				NETP_ASSERT(wp->is_idle());
				wp->set(ch_errno());
			}

			socket_base::shutdown(SHUT_WR);
			m_chflag |= int(channel_flag::F_WRITE_SHUTDOWN);
			//unset boundary
			m_chflag &= ~int(channel_flag::F_WRITE_SHUTDOWNING);
			ch_fire_write_closed();
			NETP_TRACE_SOCKET("[socket][%s]ch_do_close_write end, errno: %d, flag: %d", info().c_str(), ch_errno(), m_chflag);
			ch_rdwr_shutdown_check();
		}

		void _do_dial_done_impl( int code , fn_channel_initializer_t const& initializer, NRP<promise<int>> const& chf );

		void __do_create_accepted_socket(SOCKET nfd, address const& laddr, address const& raddr, fn_channel_initializer_t const& initializer, NRP<socket_cfg> const& cfg) {
			NRP<socket_cfg> ccfg = netp::make_ref<socket_cfg>();
			ccfg->fd = nfd;
			ccfg->family = (m_family);
			ccfg->type = (m_type);
			ccfg->proto = (m_protocol);
			ccfg->laddr = laddr;
			ccfg->raddr = raddr;

			ccfg->L = io_event_loop_group::instance()->next(L->type());
			ccfg->sockapi = cfg->sockapi;
			ccfg->option = cfg->option;
			ccfg->kvals = cfg->kvals;
			ccfg->sock_buf = cfg->sock_buf;
			ccfg->bdlimit = cfg->bdlimit;

			ccfg->L->execute([ccfg, initializer]() {
				std::tuple<int, NRP<socket>> tupc = create(ccfg);
				if (std::get<0>(tupc) != netp::OK) {
					return;
				}

				NRP<socket> const& so = std::get<1>(tupc);
				NETP_ASSERT( (so->m_chflag&int(channel_flag::F_ACTIVE)) == 0);
				so->ch_set_connected();
				so->aio_begin([so, initializer](const int aiort_) {
					int aiort = aiort_;
					if (aiort != netp::OK) {
						//begin failed
						NETP_ASSERT(so->ch_flag()&int(channel_flag::F_CLOSED));
						return;
					}
					try {
						if ( NETP_LIKELY(initializer != nullptr)) {
							initializer(so);
						}
					} catch (netp::exception const& e) {
						NETP_ASSERT(e.code() != netp::OK);
						aiort = e.code();
					} catch (std::exception const& e) {
						aiort = netp_socket_get_last_errno();
						if (aiort == netp::OK) {
							aiort = netp::E_UNKNOWN;
						}
						NETP_ERR("[socket]accept failed: %d:%s", aiort, e.what());
					} catch (...) {
						aiort = netp_socket_get_last_errno();
						if (aiort == netp::OK) {
							aiort = netp::E_UNKNOWN;
						}
						NETP_ERR("[socket]accept failed, %d: unknown", aiort );
					}

					if (aiort != netp::OK) {
						so->ch_errno() = aiort;
						so->ch_flag() |= int(channel_flag::F_READ_ERROR);
						so->ch_close_impl(nullptr);
						NETP_ERR("[socket][%s]accept failed: %d", so->info().c_str(), aiort);
						return;
					}

					NETP_ASSERT(so->m_sock_buf.rcv_size > 0, "info: %s", so->ch_info().c_str());
					NETP_ASSERT(so->m_sock_buf.snd_size > 0, "info: %s", so->ch_info().c_str());
					so->ch_fire_connected();
					so->ch_aio_read();
				});
			});
		}

		void __cb_aio_accept_impl(fn_channel_initializer_t const& fn_initializer, NRP<socket_cfg> const& ccfg, int code);
		void __cb_aio_read_impl(const int aiort_) ;
		void __cb_aio_write_impl(const int aiort_);

		//@note, we need simulate a async write, so for write operation, we'll flush outbound buffer in the next loop
		//flush until error
		//<0, is_error == (errno != E_CHANNEL_WRITING)
		//==0, flush done
		//this api would be called right after a check of writeable of the current socket
		int _do_ch_write_impl() ;
		int _do_ch_write_to_impl();

		//for connected socket type
		void _ch_do_close_listener();
		void _ch_do_close_read_write();

		void __cb_aio_notify(fn_aio_event_t const& fn_begin_done, const int aiort_) {
			NETP_ASSERT(L->in_event_loop());

			NETP_ASSERT(
				aiort_ == netp::E_IO_EVENT_LOOP_NOTIFY_TERMINATING
				||aiort_ == netp::OK
				|| aiort_ == netp::E_IO_EVENT_LOOP_TERMINATED
				|| aiort_ == netp::E_IO_EVENT_LOOP_MAXIMUM_CTX_LIMITATION
			);

			if (aiort_ == netp::E_IO_EVENT_LOOP_NOTIFY_TERMINATING) {
				//terminating notify, treat as a error
				NETP_ASSERT(m_chflag & int(channel_flag::F_IO_EVENT_LOOP_BEGIN_DONE));
				m_chflag |= (int(channel_flag::F_IO_EVENT_LOOP_NOTIFY_TERMINATING));
				m_cherrno = netp::E_IO_EVENT_LOOP_NOTIFY_TERMINATING;
				m_chflag &= ~(int(channel_flag::F_CLOSE_PENDING) | int(channel_flag::F_BDLIMIT));
				ch_close_impl(nullptr);
				return;
			}

			NETP_ASSERT(m_chflag & int(channel_flag::F_IO_EVENT_LOOP_BEGINING));
			m_chflag &= ~int(channel_flag::F_IO_EVENT_LOOP_BEGINING);

			if (aiort_ == netp::OK) {
				m_chflag |= int(channel_flag::F_IO_EVENT_LOOP_BEGIN_DONE);
			} else {
				m_chflag |= int(channel_flag::F_IO_EVENT_LOOP_BEGIN_FAILED);
				ch_errno()=(aiort_);
				ch_close_impl(nullptr);
			}
			fn_begin_done(aiort_);
		}

	public:
		NRP<promise<int>> ch_set_read_buffer_size(u32_t size) override {
			NRP<promise<int>> chp = make_ref<promise<int>>();
			L->execute([S = NRP<socket>(this), size, chp]() {
				chp->set(S->set_rcv_buffer_size(size));
			});
			return chp;
		}

		NRP<promise<int>> ch_get_read_buffer_size() override {
			NRP<promise<int>> chp = make_ref<promise<int>>();
			L->execute([S = NRP<socket>(this), chp]() {
				chp->set(S->m_sock_buf.rcv_size);
			});
			return chp;
		}

		NRP<promise<int>> ch_set_write_buffer_size(u32_t size) override {
			NRP<promise<int>> chp = make_ref<promise<int>>();
			L->execute([S = NRP<socket>(this), size, chp]() {
				chp->set(S->set_snd_buffer_size(size));
			});
			return chp;
		}

		NRP<promise<int>> ch_get_write_buffer_size() override {
			NRP<promise<int>> chp = make_ref<promise<int>>();
			L->execute([S = NRP<socket>(this), chp]() {
				chp->set(S->m_sock_buf.snd_size);
			});
			return chp;
		}

		NRP<promise<int>> ch_set_nodelay() override {
			NRP<promise<int>> chp = make_ref<promise<int>>();
			L->execute([s = NRP<socket>(this), chp]() {
				chp->set(s->turnon_nodelay());
			});
			return chp;
		}

		inline void aio_begin( fn_aio_event_t const& fn_begin_done ) {
			NETP_ASSERT(is_nonblocking());

			if (!L->in_event_loop()) {
				L->schedule([s = NRP<socket>(this), fn_begin_done]() {
					s->aio_begin(fn_begin_done);
				});
				return;
			}

#ifdef NETP_ENABLE_IOCP
			if (L->type() == T_IOCP) {
				__iocp_begin(fn_begin_done);
				return;
			}
#endif

			NETP_ASSERT( (m_chflag&(int(channel_flag::F_IO_EVENT_LOOP_BEGINING)|int(channel_flag::F_IO_EVENT_LOOP_BEGIN_DONE)|int(channel_flag::F_IO_EVENT_LOOP_BEGIN_FAILED))) == 0);
			m_chflag |= int(channel_flag::F_IO_EVENT_LOOP_BEGINING);
			L->aio_do(aio_action::BEGIN, fd(),
				std::bind(&socket::__cb_aio_notify, NRP<socket>(this),fn_begin_done,std::placeholders::_1 ));
		}

		inline void aio_end() {
			NETP_ASSERT(L->in_event_loop());
			NETP_ASSERT(m_outbound_entry_q.size() == 0);
			NETP_ASSERT(m_noutbound_bytes == 0);
			NETP_ASSERT( m_chflag&int(channel_flag::F_CLOSED));
			NETP_ASSERT( (m_chflag&(int(channel_flag::F_WATCH_READ)|int(channel_flag::F_WATCH_WRITE))) == 0 );
			NETP_TRACE_SOCKET("[socket][%s]aio_action::END, flag: %d", info().c_str(), m_chflag );
			if (m_chflag&int(channel_flag::F_IO_EVENT_LOOP_BEGIN_DONE)) {
				L->aio_do(aio_action::END, m_fd, [so = NRP<socket>(this)](const int aiort_) {
					NETP_ASSERT(aiort_ == netp::OK);
					so->ch_fire_closed(so->close());
				});
			} else {
				ch_fire_closed(close());
			}
		}

	private:
		inline void _do_aio_accept( fn_channel_initializer_t const& fn_accepted_initializer, NRP<socket_cfg> const& ccfg) {
			NETP_ASSERT(L->in_event_loop());

			if (m_chflag&int(channel_flag::F_WATCH_READ)) {
				NETP_TRACE_SOCKET("[socket][%s][_do_aio_accept]F_WATCH_READ state already", info().c_str());
				return;
			}

			if (m_chflag & int(channel_flag::F_READ_SHUTDOWN)) {
				NETP_TRACE_SOCKET("[socket][%s][_do_aio_accept]cancel for rd closed already", info().c_str());
				return;
			}

			NETP_TRACE_SOCKET("[socket][%s][_do_aio_accept]watch AIO_READ", info().c_str());
			m_chflag |= int(channel_flag::F_WATCH_READ);
#ifdef NETP_ENABLE_IOCP
			if (L->type() == T_IOCP) {
				__iocp_call_AcceptEx(fn_accepted_initializer);
				return;
			}
#endif
			//TODO: provide custome accept feature
			//const fn_aio_event_t _fn = cb_accepted == nullptr ? std::bind(&socket::__cb_async_accept_impl, NRP<socket>(this), std::placeholders::_1) : cb_accepted;
			L->aio_do(aio_action::READ, fd(),
				std::bind(&socket::__cb_aio_accept_impl,NRP<socket>(this), fn_accepted_initializer, ccfg,std::placeholders::_1));
		}

		inline void _do_aio_end_accept() {
			NETP_ASSERT(L->in_event_loop());
#ifdef NETP_IO_MODE_IOCP
			if (L->type() == T_IOCP) {
				return;
			}
#endif
			ch_aio_end_read();
		}

#ifdef NETP_IO_MODE_IOCP
		inline void __iocp_begin( fn_aio_event_t const& fn_begin_done ) {
			L->iocp_call(iocp_action::BEGIN, fd(), nullptr, [so=NRP<socket>(this), fn_begin_done](const int aiort_) {
				if (aiort == netp::OK) {
					if (!so->is_listener()) {
						so->L->iocp_call(iocp_action::WRITE, so->fd(), std::bind(&socket::__iocp_call_WSASend_set_ol, so, std::placeholders::_1),
							std::bind(&socket::__iocp_call_WSASend_done, so, std::placeholders::_1));
					}
				}
				fn_begin_done == nullptr ? 0 : fn_begin_done(r);
			});
		}

		inline SOCKET __iocp_call_AcceptEx_create_fd(void* ol_) {
			NETP_ASSERT(L->in_event_loop());
			(void)ol_;
			return netp::socket(netp::fncfg[m_fn_api_type], m_family, m_type, m_protocol);
		}

		void __iocp_call_AcceptEx_done(const int aiort_, fn_channel_initializer_t const& fn_accepted_initializer) {
			NETP_ASSERT(L->in_event_loop());

			//NETP_ASSERT(m_fn_accept_initializer != nullptr);
			int ec = aiort;

			if (ec == netp::OK) {
				const SOCKET& nfd = r.fd;
				NETP_ASSERT(nfd != NETP_INVALID_SOCKET);
				SOCKADDR_IN* raddr_in = nullptr;
				SOCKADDR_IN* laddr_in = nullptr;
				int raddr_in_len = sizeof(SOCKADDR_IN);
				int laddr_in_len = sizeof(SOCKADDR_IN);

				LPFN_GETACCEPTEXSOCKADDRS fn_getacceptexsockaddrs = (LPFN_GETACCEPTEXSOCKADDRS)netp::os::load_api_ex_address(netp::os::API_GET_ACCEPT_EX_SOCKADDRS);
				NETP_ASSERT(fn_getacceptexsockaddrs != 0);
				fn_getacceptexsockaddrs(r.buf, 0,
					sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
					(LPSOCKADDR*)&laddr_in, &laddr_in_len,
					(LPSOCKADDR*)&raddr_in, &raddr_in_len);

				const address raddr(*raddr_in);
				const address laddr(*laddr_in);

				if (raddr == laddr) {
					NETP_WARN("[socket][%s][accept]raddr == laddr, force close: %u", info().c_str(), nfd);
					NETP_CLOSE_SOCKET(nfd);
				} else {
					NETP_ASSERT(laddr.port() == m_laddr.port());
					NETP_ASSERT(raddr_in->sin_family == OS_DEF_family[m_family]);

					try {
						__accept_new_fd(nfd, laddr, raddr, fn_accepted_initializer);
					}catch (netp::exception & e) {
						NETP_ERR("[#%d]accept new fd exception: [%d]%s\n%s(%d) %s\n%s", m_fd,
							e.code(), e.what(), e.file(), e.line(), e.function(), e.callstack());
						NETP_CLOSE_SOCKET(nfd);
					}
					catch (std::exception& e) {
						NETP_ERR("[#%d]accept new fd exception, e: %s", m_fd, e.what());
						NETP_CLOSE_SOCKET(nfd);
					} catch (...) {
						NETP_ERR("[#%d]accept new fd exception, e: %d", m_fd, netp_socket_get_last_errno());
						NETP_CLOSE_SOCKET(nfd);
					}
				}
			}
			if (IS_ERRNO_EQUAL_WOULDBLOCK(ec) || ec == netp::OK) {
				return;
			}
			ch_close();
		}

		inline void __iocp_call_AcceptEx(fn_channel_initializer_t const& fn_accepted_initializer) {
			L->iocp_call(iocp_action::ACCEPT, m_fd,
				std::bind(&socket::__iocp_call_AcceptEx_create_fd, NRP<socket>(this), std::placeholders::_1),
				[fn_accepted_initializer, so = NRP<socket>(this)](const int aiort_) {
					so->__iocp_call_AcceptEx_done(r, fn_accepted_initializer);
				});
		}

		inline overlapped_return_t __iocp_call_do_ConnectEx(void* ol_) {
			NETP_ASSERT(L->in_event_loop());

			WSAOVERLAPPED* ol = (WSAOVERLAPPED*)ol_;
			NETP_ASSERT(!m_raddr.is_null());

			sockaddr_in addr;
			::memset(&addr, 0,sizeof(addr));

			addr.sin_family = OS_DEF_family[m_family];
			addr.sin_port = m_raddr.nport();
			addr.sin_addr.s_addr = m_raddr.nipv4();
			int socklen = sizeof(addr);
			const LPFN_CONNECTEX fn_connectEx = (LPFN_CONNECTEX)netp::os::load_api_ex_address(netp::os::API_CONNECT_EX);
			NETP_ASSERT(fn_connectEx != 0);

			const BOOL connrt = fn_connectEx(m_fd,(SOCKADDR*)(&addr), socklen,nullptr, 0, nullptr, ol);
			if (connrt == FALSE) {
				int ec = netp_socket_get_last_errno();
				if (ec != netp::E_ERROR_IO_PENDING) {
					NETP_DEBUG("[socket][iocp][#%u]socket __connectEx failed", m_fd, connrt);
					return ec;
				}
			}
			NETP_DEBUG("[socket][iocp][#%u]connectex ok", m_fd );
			return netp::OK;
		}

		inline void __iocp_call_ConnectEx(fn_aio_event_t const& fn ) {
			L->iocp_call(iocp_action::CONNECT, m_fd, std::bind(&socket::__iocp_call_do_ConnectEx, NRP<socket>(this), std::placeholders::_1), fn );
		}

		void __iocp_WSARead_done(const int aiort_) {
			NETP_ASSERT(L->in_event_loop());
			NETP_ASSERT(!is_listener());
			int ec = r.v.len;

			if (NETP_LIKELY(ec) > 0) {
				NRP<packet> income = netp::make_ref<packet>(ec);
				income->write((byte_t*)r.buf, r.v.len);
				channel::ch_read(income);
				ec = netp::OK;
			}
			else if (ec == 0) {
				ec = netp::E_SOCKET_GRACE_CLOSE;
			} else {
				NETP_TRACE_SOCKET("[socket][%s]WSARead error: %d", info().c_str(), ec);
			}

			switch (ec) {
			case netp::OK:
			case netp::E_SOCKET_READ_BLOCK:
			{}
			break;
			case netp::E_SOCKET_GRACE_CLOSE:
			{
				ch_close_read();
			}
			break;
			default:
			{
				NETP_TRACE_SOCKET("[socket][%s]async read, error: %d, close", info().c_str(), ec);
				ch_errno(ec);
				ch_close();
			}
			}
		}

		//non null, send
		//null clear
		inline int __iocp_call_WSASend_set_ol( void* ol_ ) {
			NETP_ASSERT(L->in_event_loop());
			WSAOVERLAPPED* ol = (WSAOVERLAPPED*)ol_;
			NETP_ASSERT(m_ol_write == 0);
			m_ol_write = ol;
			return netp::OK;
		}

		inline void __iocp_call_WSASend_done(const int aiort_) {
			NETP_ASSERT(L->in_event_loop());

			int const& len = r.v.len;
			NETP_ASSERT((m_chflag&channel_flag::F_WATCH_WRITE) != 0);
			if (len<0) {
				m_chflag &= ~channel_flag::F_WATCH_WRITE;
				socket::__async_flush_done(aiort);
				return;
			}

			NETP_ASSERT(m_outbound_entry_q.size());
			NETP_ASSERT(m_noutbound_bytes > 0);
			socket_outbound_entry entry = m_outbound_entry_q.front();
			NETP_ASSERT(entry.data != nullptr);
			entry.data->skip(len);
			m_noutbound_bytes -= len;
			if (entry.data->len() == 0) {
				entry.chp->set(netp::OK);
				m_outbound_entry_q.pop_front();
			}
			socket::__async_flush_done(netp::OK);
			if ((m_noutbound_bytes>0)) {
				NETP_ASSERT((m_chflag&channel_flag::F_WATCH_WRITE) != 0);
				__IOCP_do_ch_flush_impl();
			} else {
				m_chflag &= ~channel_flag::F_WATCH_WRITE;
			}
		}

		//one shot one packet
		inline void __IOCP_do_ch_flush_impl() {
			NETP_ASSERT(L->in_event_loop());
			NETP_ASSERT(m_ol_write != 0);
			int wrt = netp::OK;
			while (m_outbound_entry_q.size()) {
				NETP_ASSERT(m_ol_write != nullptr);
				NETP_ASSERT(m_noutbound_bytes > 0);
				socket_outbound_entry& entry = m_outbound_entry_q.front();
				NRP<packet>& outlet = entry.data;
				::memset(m_ol_write, 0, sizeof(*m_ol_write));
				WSABUF wsabuf = { outlet->len(), (char*)outlet->head() };
				wrt = ::WSASend(m_fd, &wsabuf, 1, nullptr, 0, m_ol_write, nullptr);
				if (wrt == netp::OK) {
					m_chflag |= channel_flag::F_WATCH_WRITE;
					break;
				}
				NETP_ASSERT(wrt == NETP_SOCKET_ERROR);
				wrt = netp_socket_get_last_errno();
				if (wrt == netp::E_ERROR_IO_PENDING) {
					m_chflag |= channel_flag::F_WATCH_WRITE;
					break;
				}

				_async_io_begin_write();
				m_chflag |= channel_flag::F_WRITE_ERROR;
				ch_errno(wrt);
				ch_close();
				break;
			}
		}
#endif
	public:
		__NETP_FORCE_INLINE channel_id_t ch_id() const override { return m_fd; }
		std::string ch_info() const override { return info();}

		void ch_set_bdlimit(u32_t limit) override {
			L->execute([s = NRP<socket>(this), limit]() {
				s->m_outbound_limit = limit;
				s->m_outbound_budget = s->m_outbound_limit;
			});
		};

		void ch_write_impl(NRP<packet> const& outlet, NRP<promise<int>> const& chp) override ;
		void ch_write_to_impl(NRP<packet> const& outlet, netp::address const& to, NRP<promise<int>> const& chp) override;

		void ch_close_read_impl(NRP<promise<int>> const& closep) override
		{
			NETP_ASSERT(L->in_event_loop());
			NETP_TRACE_SOCKET("[socket][%s]ch_close_read_impl, _ch_do_close_read, errno: %d, flag: %d", info().c_str(), ch_errno(), m_chflag);
			int prt = netp::OK;
			if (m_chflag & (int(channel_flag::F_READ_SHUTDOWNING)|int(channel_flag::F_CLOSE_PENDING)|int(channel_flag::F_CLOSING)) ) {
				prt=(netp::E_OP_INPROCESS);
			} else if ((m_chflag&int(channel_flag::F_READ_SHUTDOWN)) != 0) {
				prt=(netp::E_CHANNEL_WRITE_CLOSED);
			} else {
				_ch_do_close_read();
			}

			if (closep) { closep->set(prt); }
		}

		void ch_close_write_impl(NRP<promise<int>> const& chp) override;
		void ch_close_impl(NRP<promise<int>> const& chp) override;

		void ch_aio_read(fn_aio_event_t const& fn_read = nullptr) {
			if (!L->in_event_loop()) {
				L->schedule([s = NRP<socket>(this), fn_read]()->void {
					s->ch_aio_read(fn_read);
				});
				return;
			}
			NETP_ASSERT( (m_chflag&int(channel_flag::F_READ_SHUTDOWNING)) == 0 );
			if (m_chflag&int(channel_flag::F_WATCH_READ)) {
				NETP_TRACE_SOCKET("[socket][%s]aio_action::READ, ignore, flag: %d", info().c_str(), m_chflag );
				return;
			}

			if (m_chflag&int(channel_flag::F_READ_SHUTDOWN)) {
				NETP_ASSERT((m_chflag & int(channel_flag::F_WATCH_READ)) == 0);
				if (fn_read != nullptr) {
					fn_read(netp::E_CHANNEL_READ_CLOSED);
				}
				return;
			}

			m_chflag |= int(channel_flag::F_WATCH_READ);

#ifdef NETP_IO_MODE_IOCP
			if (L->type() == T_IOCP) {
				const fn_aio_event_t _fn_io = fn_read == nullptr ? std::bind(&socket::__iocp_WSARead_done, NRP<socket>(this), std::placeholders::_1) : fn_read;
				L->iocp_call(iocp_action::READ, m_fd, nullptr, _fn_io);
				return;
			}
#endif

			fn_read == nullptr ?
			L->aio_do(aio_action::READ, m_fd, std::bind(&socket::__cb_aio_read_impl, NRP<socket>(this), std::placeholders::_1)) :
			L->aio_do(aio_action::READ, m_fd, fn_read);

			NETP_TRACE_SOCKET("[socket][%s]aio_action::READ", info().c_str() );
		}

		void ch_aio_end_read() {
			if (!L->in_event_loop()) {
				L->schedule([_so = NRP<socket>(this)]()->void {
					_so->ch_aio_end_read();
				});
				return;
			}

			if ((m_chflag&int(channel_flag::F_WATCH_READ))) {
				m_chflag &= ~int(channel_flag::F_WATCH_READ);
				L->aio_do(aio_action::END_READ, m_fd,nullptr );
				NETP_TRACE_SOCKET("[socket][%s]aio_action::END_READ", info().c_str());
			}
		}

		void ch_aio_write(fn_aio_event_t const& fn_write = nullptr) override {
			if (!L->in_event_loop()) {
				L->schedule([_so = NRP<socket>(this), fn_write]()->void {
					_so->ch_aio_write(fn_write);
				});
				return;
			}

			if (m_chflag&int(channel_flag::F_WATCH_WRITE)) {
				NETP_ASSERT(m_chflag & int(channel_flag::F_CONNECTED));
				//NETP_TRACE_SOCKET("[socket][%s]aio_action::WRITE, ignore for write watch already", info().c_str());
				return;
			}

			if (m_chflag&int(channel_flag::F_WRITE_SHUTDOWN)) {
				NETP_ASSERT((m_chflag & int(channel_flag::F_WATCH_WRITE)) == 0);
				NETP_TRACE_SOCKET("[socket][%s]aio_action::WRITE, cancel for wr closed already", info().c_str());
				if (fn_write != nullptr) {
					fn_write(netp::E_CHANNEL_WRITE_CLOSED);
				}
				return;
			}

#ifdef NETP_IO_MODE_IOCP
			if (L->type() == T_IOCP) {
				NETP_ASSERT((m_chflag&channel_flag::F_WATCH_WRITE) == 0);
				__IOCP_do_ch_flush_impl();
				return;
			}
#endif

			m_chflag |= int(channel_flag::F_WATCH_WRITE);
			fn_write == nullptr ?
				L->aio_do(aio_action::WRITE, m_fd, std::bind(&socket::__cb_aio_write_impl, NRP<socket>(this), std::placeholders::_1)) :
				L->aio_do(aio_action::WRITE, m_fd, fn_write) ;

			NETP_TRACE_SOCKET("[socket][%s]aio_action::WRITE", info().c_str());
		}

		void ch_aio_end_write() override {
			if (!L->in_event_loop()) {
				L->schedule([_so = NRP<socket>(this)]()->void {
					_so->ch_aio_end_write();
				});
				return;
			}

			if (m_chflag&int(channel_flag::F_WATCH_WRITE)) {
				m_chflag &= ~int(channel_flag::F_WATCH_WRITE);

#ifdef NETP_IO_MODE_IOCP
				if (L->type() == T_IOCP) {
					return;
				}
#endif

				L->aio_do(aio_action::END_WRITE, m_fd,nullptr);
				NETP_TRACE_SOCKET("[socket][%s]aio_action::END_WRITE", info().c_str());
			}
		}

		void ch_aio_connect(fn_aio_event_t const& fn = nullptr) override {
			NETP_ASSERT(fn != nullptr);

#ifdef NETP_IO_MODE_IOCP
			if (L->type() == T_IOCP) {
				__iocp_call_ConnectEx(fn);
				return;
			}
#endif
			ch_aio_write(fn);
		}
		void ch_aio_end_connect() override {
#ifdef NETP_IO_MODE_IOCP
			if (L->type() == T_IOCP) {
				return;
			}
#endif
			NETP_ASSERT(!ch_is_passive());
			ch_aio_end_write();
		}
	};
}
#endif
