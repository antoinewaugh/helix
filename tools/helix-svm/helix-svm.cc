/*
 * helix-svm - Order book events for libsvm
 *
 * This is a program that listens to a market data feed and outputs order book
 * events in libsvm compatible format.
 *
 * Feature extraction and classification model is based on the following paper:
 *
 *   Kercheval, Alec N., and Yuan Zhang. "Modeling high-frequency limit order
 *   book dynamics with support vector machines." (2013).
 */

#include <helix-c/helix.h>
#include <getopt.h>
#include <libgen.h>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <vector>
#include <deque>
#include <cmath>
#include <tuple>
#include <uv.h>

using namespace std;

static const char *program;

FILE* output;

struct config {
	const char *symbol;
	const char *multicast_proto;
	const char *multicast_addr;
	int multicast_port;
	const char *output;
};

class svm_session {
	static constexpr size_t nr_levels = 5;

	struct features {
		std::vector<double> values;

		void add(double v) {
			values.push_back(v);
		}
	};

	// Type of a time point (in number of events):
	using time_point = uint64_t;

	// Type of duration (in number of events):
	using duration = uint64_t;

	// Type of mid price movement:
	enum class label_type : int {
		upward		= +1,
		downward	= -1,
		stationary	= 0,
	};

	// Current time:
	time_point current_time = 0;

	// Look-ahead interval ahead of current time:
	static constexpr duration lookahead_interval = 5;

	// Lookback buffer of (midprice, features) tuple before current time:
	deque<tuple<uint64_t, features>> lookback;

public:
	void process_ob_event(helix_order_book_t ob) {
		if (helix_order_book_ask_levels(ob) < nr_levels && helix_order_book_bid_levels(ob) < nr_levels) {
			return;
		}
		uint64_t midprice = helix_order_book_midprice(ob, 0);
		auto curr_features = std::move(extract(ob));
		if (lookback.empty()) {
			lookback.emplace_back(make_tuple(midprice, curr_features));
			return;
		}
		uint64_t prev_midprice = helix_order_book_midprice(ob, 0);
		features prev_features;
		tie(prev_midprice, prev_features) = lookback.front();

		if (curr_features.values == prev_features.values) {
			return;
		}
		lookback.emplace_back(make_tuple(midprice, curr_features));

		if (lookback.size() < lookahead_interval) {
			return;
		}
		lookback.pop_front();

		fmt(label(prev_midprice, midprice), prev_features);
	}
private:
	features extract(helix_order_book_t ob) {
		features result{};
		for (size_t i = 0; i < nr_levels; i++) {
			auto bid_price = helix_order_book_bid_price(ob, i) / 10000.0;
			auto ask_price = helix_order_book_ask_price(ob, i) / 10000.0;
			auto bid_size = helix_order_book_bid_size(ob, i);
			auto ask_size = helix_order_book_ask_size(ob, i);
			auto midprice = helix_order_book_midprice(ob, i) / 10000.0;
			// Price and volume (n levels):
			result.add(ask_price);
			result.add(ask_size);
			result.add(bid_price);
			result.add(bid_size);
			// Bid-ask spread and mid-prices:
			result.add(ask_price - bid_price);
			result.add(midprice);
		}
		for (size_t i = 1; i < nr_levels; i++) {
			// Price differences:
			auto bid0 = helix_order_book_midprice(ob, i-1) / 10000.0;
			auto bid1 = helix_order_book_midprice(ob, i) / 10000.0;
			auto ask0 = helix_order_book_midprice(ob, i-1) / 10000.0;
			auto ask1 = helix_order_book_midprice(ob, i) / 10000.0;
			result.add(abs(ask1-ask0));
			result.add(abs(bid1-bid0));
		}
		return result;
	}

	label_type label(uint64_t prev_midprice, uint64_t midprice) {
		if (midprice > prev_midprice) {
			return label_type::upward;
		} else if (midprice < prev_midprice) {
			return label_type::downward;
		} else {
			return label_type::stationary;
		}
	}

	void fmt(label_type label, const features& features) {
		cout << int(label);
		for (size_t i = 0; i < features.values.size(); i++) {
			cout << " " << i+1 << ":" << features.values[i];
		}
		cout << endl;
	}
};

static struct svm_session svm_session;

static uv_buf_t alloc_packet(uv_handle_t* handle, size_t suggested_size)
{
	static char rx_buffer[65536];

	return uv_buf_init(rx_buffer, sizeof(rx_buffer));
}

struct svm_fmt_ops *fmt_ops;

static void process_ob_event(helix_session_t session, helix_order_book_t ob)
{
	struct svm_session* s = reinterpret_cast<struct svm_session*>(helix_session_data(session));

	s->process_ob_event(ob);
}

static void process_trade_event(helix_session_t session, helix_trade_t trade)
{
}

static void recv_packet(uv_udp_t* handle, ssize_t nread, uv_buf_t buf, struct sockaddr* addr, unsigned flags)
{
	if (nread > 0) {
		helix_session_process_packet(reinterpret_cast<helix_session_t>(handle->data), buf.base, nread);
	}
}

static void libuv_error(const char *s)
{
	uv_err_t err;

	err = uv_last_error(uv_default_loop());

	fprintf(stderr, "error: %s: %s (%s)\n", s, uv_strerror(err), uv_err_name(err));
	exit(1);
}

static void usage(void)
{
	fprintf(stdout,
		"usage: %s [options]\n"
		"  options:\n"
		"    -s, --symbol symbol          Ticker symbol to listen to.\n"
		"    -c, --multicast-proto proto  UDP multicast protocol listen to.\n"
		"    -a, --multicast-addr addr    UDP multicast address to listen to.\n"
		"    -p, --multicast-port port    UDP multicast port to listen to.\n"
		"    -o, --output filename        Output filename.\n"
		"    -h, --help                   display this help and exit\n",
		program);
	exit(1);
}

static struct option svm_options[] = {
	{"symbol",          required_argument, 0, 's'},
	{"multicast-proto", required_argument, 0, 'c'},
	{"multicast-addr",  required_argument, 0, 'a'},
	{"multicast-port",  required_argument, 0, 'p'},
	{"output",          required_argument, 0, 'o'},
	{"help",            no_argument,       0, 'h'},
	{0, 0, 0, 0}
};

static void parse_options(struct config *cfg, int argc, char *argv[])
{
	for (;;) {
		int opt_idx = 0;
		int c;

		c = getopt_long(argc, argv, "s:c:a:o:p:h", svm_options, &opt_idx);
		if (c == -1)
			break;

		switch (c) {
		case 's':
			cfg->symbol = optarg;
			break;
		case 'c':
			cfg->multicast_proto = optarg;
			break;
		case 'a':
			cfg->multicast_addr = optarg;
			break;
		case 'o':
			cfg->output = optarg;
			break;
		case 'p':
			cfg->multicast_port = strtol(optarg, NULL, 10);
			break;
		case 'h':
			usage();
		default:
			usage();
		}
	}
}

int main(int argc, char *argv[])
{
	helix_session_t session;
	struct sockaddr_in addr;
	helix_protocol_t proto;
	struct config cfg = {};
	uv_udp_t socket;
	int err;

	program = basename(argv[0]);

	parse_options(&cfg, argc, argv);

	if (!cfg.symbol) {
		fprintf(stderr, "error: symbol is not specified. Use the '-s' option to specify it.\n");
		exit(1);
	}

	if (!cfg.multicast_proto) {
		fprintf(stderr, "error: multicast protocol is not specified. Use the '-c' option to specify it.\n");
		exit(1);
	}

	if (!cfg.multicast_addr) {
		fprintf(stderr, "error: multicast address is not specified. Use the '-a' option to specify it.\n");
		exit(1);
	}

	if (!cfg.multicast_port) {
		fprintf(stderr, "error: multicast port is not specified. Use the '-p' option to specify it.\n");
		exit(1);
	}

	if (cfg.output) {
		output = fopen(cfg.output, "w");
		if (!output) {
			fprintf(stderr, "error: %s: %s\n", cfg.output, strerror(errno));
			exit(1);
		}
	} else {
		output = stdout;
	}

	proto = helix_protocol_lookup(cfg.multicast_proto);
	if (!proto) {
		fprintf(stderr, "error: protocol '%s' is not supported\n", cfg.multicast_proto);
		exit(1);
	}

	session = helix_session_create(proto, cfg.symbol, process_ob_event, process_trade_event, &svm_session);
	if (!session) {
		fprintf(stderr, "error: unable to create new session\n");
		exit(1);
	}

	err = uv_udp_init(uv_default_loop(), &socket);
	if (err) {
		libuv_error("uv_udp_init");
	}
	socket.data = session;

	addr = uv_ip4_addr("0.0.0.0", cfg.multicast_port);

	err = uv_udp_bind(&socket, addr, 0);
	if (err) {
		libuv_error("uv_udp_bind");
	}

	err = uv_udp_set_membership(&socket, cfg.multicast_addr, NULL, UV_JOIN_GROUP);
	if (err) {
		libuv_error("uv_udp_set_membership");
	}

	err = uv_udp_recv_start(&socket, alloc_packet, recv_packet);
	if (err) {
		libuv_error("uv_udp_recv_start");
	}

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);
}