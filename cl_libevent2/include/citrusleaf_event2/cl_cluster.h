/*
 * The Aerospike C interface. A good, basic library that many clients can be based on.
 *
 * This is the internal, non-public header file.
 *
 * this code currently assumes that the server is running in an ASCII-7 based
 * (ie, utf8 or ISO-LATIN-1)
 * character set, as values coming back from the server are UTF-8. We currently
 * don't bother to convert to the character set of the machine we're running on
 * but we advertise these values as 'strings'
 *
 * All rights reserved
 * Brian Bulkowski, 2009
 * CitrusLeaf
 */


// do this both the new skool and old skool way which gives the highest correctness,
// speed, and compatibility
#pragma once

#include <pthread.h>
#include <stdint.h>

#include "citrusleaf/cf_atomic.h"
#include "citrusleaf/cf_base_types.h"
#include "citrusleaf/cf_digest.h"
#include "citrusleaf/cf_ll.h"
#include "citrusleaf/cf_queue.h"
#include "citrusleaf/cf_vector.h"
#include "citrusleaf/proto.h"

#include "ev2citrusleaf.h"


#ifdef __cplusplus
extern "C" {
#endif

struct sockaddr_in;

#define CLUSTER_NODE_MAGIC 0x9B00134C

typedef enum {
	INFO_REQ_NONE			= 0,
	INFO_REQ_CHECK			= 1,
	INFO_REQ_GET_REPLICAS	= 2
} node_info_req_type;

#define NODE_INFO_REQ_MAX_INTERVALS 5

// Must be >= longest "names" string sent in a node info request.
#define INFO_STR_MAX_LEN 64

typedef struct node_info_req_s {
	// What type of info request is in progress, if any.
	node_info_req_type		type;

	// How many node timer periods this request has lasted.
	uint32_t				intervals;

	// Buffer for writing to socket.
	uint8_t					wbuf[sizeof(cl_proto) + INFO_STR_MAX_LEN];
	size_t					wbuf_size;
	size_t					wbuf_pos;

	// Buffer for reading proto header from socket.
	uint8_t					hbuf[sizeof(cl_proto)];
	size_t					hbuf_pos;

	// Buffer for reading proto body from socket.
	uint8_t*				rbuf;
	size_t					rbuf_size;
	size_t					rbuf_pos;
} node_info_req;

typedef struct cl_cluster_node_s {
	// Sanity-checking field.
	uint32_t				MAGIC;

	// This node's name, a null-terminated hex string.
	char					name[20];

	// A vector of sockaddr_in which the host (node) is currently known by.
	cf_vector				sockaddr_in_v;

	// The cluster we belong to.
	ev2citrusleaf_cluster*	asc;

	// This node's health.
	cf_atomic_int			dunned;
	cf_atomic_int			dun_count;

	// Socket pool for (non-info) transactions on this node.
	cf_queue*				conn_q;

	// What version of partition information we have for this node.
	cf_atomic_int			partition_generation;

	// Socket for info transactions on this node.
	int						info_fd;

	// The info transaction in progress, if any.
	node_info_req			info_req;

	// Space for two events: periodic trigger timer, and info request.
	uint8_t					event_space[];
} cl_cluster_node;


#define MAX_REPLICA_COUNT 5

typedef struct cl_partition_s {
	void *lock;
	cl_cluster_node	*write;
	int n_read;
	cl_cluster_node *read[MAX_REPLICA_COUNT];
} cl_partition;


typedef struct cl_partition_table_s {
	
	struct cl_partition_table_s *next;
	
	char ns[33];  // the namespace name

	bool was_dumped; // only dump table if it changed since last time

	cl_partition partitions[];
	
} cl_partition_table;


#define CLUSTER_MAGIC 0x91916666

struct ev2citrusleaf_cluster_s {
	
	cf_ll_element		ll_e; // global list of all clusters - good for debugging
	
	uint32_t    MAGIC;
	
	bool		follow;   // possible to create a no-follow cluster
						  // mostly for testing
						  // that only targets specific nodes
						  
	// AKG - multi-thread access, shutdown usage
	bool		shutdown; // we might be in shutdown phase, don't start more info
							// requests or similar

	pthread_t			mgr_thread;		// (optional) internally created cluster manager thread
	bool				internal_mgr;	// is there an internally created cluster manager thread and base?
	struct event_base	*base;			// cluster manager base, specified by app or internally created
	struct evdns_base	*dns_base;

	// Cluster-specific functionality options.
	ev2citrusleaf_cluster_options	options;

	// List of host-strings added by the user.
	cf_vector		host_str_v;	// vector is pointer-type
	cf_vector		host_port_v;  // vector is integer-type
	
	// actual **node objects** that represent the cluster
	void 			*node_v_lock;
	cf_atomic_int	last_node;
	cf_vector		node_v;      // vector is pointer-type, host objects are ref-counted

	// There are occasions where we want to queue pending transactions until
	// nodes come available (like, embarrassingly, the first request).
	cf_queue	*request_q;
	void 		*request_q_lock;

	// Transactions in progress. Includes transactions in the request queue
	// above (everything needing a callback). No longer used for clean shutdown
	// other than to issue a warning if there are incomplete transactions.
	cf_atomic_int	requests_in_progress; 

	// Internal non-node info requests in progress, used for clean shutdown.
	cf_atomic_int	pings_in_progress;

	// information about where all the partitions are
	// AKG - multi-thread access, but never changes on server
	cl_partition_id		n_partitions;
	// AKG - pointer set/read only in cluster thread
	cl_partition_table *partition_table_head;
	
	// statistics?
	
	uint8_t 	event_space[];
	
};


typedef enum {
	DONT_DUN				= -1,
	// 0 onwards MUST correspond to strings in cl_cluster_dun_human[].

	DUN_BAD_NAME			= 0, // node name changed
	DUN_NO_SOCKADDR			= 1, // node has no socket addresses

	// Ordinary transactions (and batch transactions for now)
	DUN_CONNECT_FAIL		= 2, // new pool socket can't start connect
	DUN_RESTART_FD			= 3, // existing pool socket check gives error
	DUN_NETWORK_ERROR		= 4, // error during ordinary transaction
	DUN_USER_TIMEOUT		= 5, // ordinary transaction timed out

	// Node info requests
	DUN_INFO_CONNECT_FAIL	= 6, // node info socket can't start connect
	DUN_INFO_RESTART_FD		= 7, // existing node info socket check gives error
	DUN_INFO_NETWORK_ERROR	= 8, // error during node info request
	DUN_INFO_TIMEOUT		= 9, // node info request timed out
} cl_cluster_dun_type;


//
// a global list of all clusters is interesting sometimes
//
// AKG - only changed in create/destroy, read in print_stats
extern cf_ll		cluster_ll;


// Do a lookup with this name and port, and add the sockaddr to the
// vector using the unique lookup
extern int cl_lookup_immediate(char *hostname, short port, struct sockaddr_in *sin);
typedef void (*cl_lookup_async_fn) (int result, cf_vector *sockaddr_in_v, void *udata);
extern int cl_lookup(struct evdns_base *base, char *hostname, short port, cl_lookup_async_fn cb, void *udata);

// Cluster calls
extern cl_cluster_node *cl_cluster_node_get(ev2citrusleaf_cluster *asc, const char *ns, const cf_digest *d, bool write);  // get node from cluster
extern void cl_cluster_node_release(cl_cluster_node *cn, char *msg);
extern void cl_cluster_node_reserve(cl_cluster_node *cn, char *msg);
extern void cl_cluster_node_put(cl_cluster_node *cn);          // put node back
extern void cl_cluster_node_dun(cl_cluster_node *cn, cl_cluster_dun_type dun);			// node is bad!
extern void cl_cluster_node_ok(cl_cluster_node *cn);			// node is good!
extern int cl_cluster_node_fd_get(cl_cluster_node *cn);			// get an FD to the node
extern void cl_cluster_node_fd_put(cl_cluster_node *cn, int fd); // put the FD back

//
extern int citrusleaf_info_host(struct sockaddr_in *sa_in, char *names, char **values, int timeout_ms);
extern int citrusleaf_info_parse_single(char *values, char **value);

extern int citrusleaf_cluster_init();
extern int citrusleaf_cluster_shutdown();

// Partition table calls
// --- all these assume the partition lock is held
extern void cl_partition_table_remove_node( ev2citrusleaf_cluster *asc, cl_cluster_node *node );
extern void cl_partition_table_destroy_all(ev2citrusleaf_cluster *asc);
extern void cl_partition_table_set( ev2citrusleaf_cluster *asc, cl_cluster_node *node, const char *ns, cl_partition_id pid, bool write);
extern cl_cluster_node *cl_partition_table_get( ev2citrusleaf_cluster *asc, const char *ns, cl_partition_id pid, bool write);
extern void cl_partition_table_dump(ev2citrusleaf_cluster* asc);

#ifdef __cplusplus
} // end extern "C"
#endif


