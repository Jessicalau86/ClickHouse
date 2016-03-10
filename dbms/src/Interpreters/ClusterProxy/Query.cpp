#include <DB/Interpreters/ClusterProxy/Query.h>
#include <DB/Interpreters/ClusterProxy/IQueryConstructor.h>
#include <DB/Interpreters/Settings.h>
#include <DB/Interpreters/Context.h>
#include <DB/Interpreters/Cluster.h>
#include <DB/Interpreters/IInterpreter.h>
#include <DB/DataStreams/RemoteBlockInputStream.h>


namespace DB
{

namespace ClusterProxy
{

Query::Query(IQueryConstructor & query_constructor_, const Cluster & cluster_,
	ASTPtr query_ast_, const Context & context_, const Settings & settings_, bool enable_shard_multiplexing_)
	: query_constructor(query_constructor_), cluster(cluster_), query_ast(query_ast_),
	context(context_), settings(settings_), enable_shard_multiplexing(enable_shard_multiplexing_)
{
}

BlockInputStreams Query::execute()
{
	BlockInputStreams res;

	const std::string query = queryToString(query_ast);

	Settings new_settings = settings;
	new_settings.queue_max_wait_ms = Cluster::saturate(new_settings.queue_max_wait_ms, settings.limits.max_execution_time);
	/// Не имеет смысла на удалённых серверах, так как запрос отправляется обычно с другим user-ом.
	new_settings.max_concurrent_queries_for_user = 0;

	/// Ограничение сетевого трафика, если нужно.
	ThrottlerPtr throttler;
	if (settings.limits.max_network_bandwidth || settings.limits.max_network_bytes)
		throttler.reset(new Throttler(
			settings.limits.max_network_bandwidth,
			settings.limits.max_network_bytes,
			"Limit for bytes to send or receive over network exceeded."));

	/// Распределить шарды равномерно по потокам.

	size_t remote_count = 0;

	if (query_constructor.isInclusive())
	{
		for (const auto & shard_info : cluster.getShardsInfo())
		{
			if (shard_info.hasRemoteConnections())
				++remote_count;
		}
	}
	else
		remote_count = cluster.getRemoteShardCount();

	size_t thread_count;

	if (!enable_shard_multiplexing)
		thread_count = remote_count;
	else if (remote_count == 0)
		thread_count = 0;
	else if (settings.max_distributed_processing_threads == 0)
		thread_count = 1;
	else
		thread_count = std::min(remote_count, static_cast<size_t>(settings.max_distributed_processing_threads));

	size_t pools_per_thread = (thread_count > 0) ? (remote_count / thread_count) : 0;
	size_t remainder = (thread_count > 0) ? (remote_count % thread_count) : 0;

	bool do_init = true;

	/// Compute the number of parallel streams.
	size_t stream_count = 0;
	size_t pool_count = 0;
	size_t current_thread = 0;

	for (const auto & shard_info : cluster.getShardsInfo())
	{
		bool create_local_queries = shard_info.isLocal();
		bool create_remote_queries = query_constructor.isInclusive() ? shard_info.hasRemoteConnections() : !create_local_queries;

		if (create_local_queries)
			stream_count += shard_info.local_addresses.size();

		if (create_remote_queries)
		{
			size_t excess = (current_thread < remainder) ? 1 : 0;
			size_t actual_pools_per_thread = pools_per_thread + excess;

			if (actual_pools_per_thread == 1)
			{
				++stream_count;
				++current_thread;
			}
			else
			{
				if (do_init)
				{
					pool_count = 0;
					do_init = false;
				}

				++pool_count;
				if (pool_count == actual_pools_per_thread)
				{
					++stream_count;
					do_init = true;
					++current_thread;
				}
			}
		}
	}

	query_constructor.setupBarrier(stream_count);

	/// Цикл по шардам.
	ConnectionPoolsPtr pools;
	do_init = true;
	current_thread = 0;

	for (const auto & shard_info : cluster.getShardsInfo())
	{
		bool create_local_queries = shard_info.isLocal();
		bool create_remote_queries = query_constructor.isInclusive() ? shard_info.hasRemoteConnections() : !create_local_queries;

		if (create_local_queries)
		{
			/// Добавляем запросы к локальному ClickHouse.

			DB::Context new_context = context;
			new_context.setSettings(new_settings);

			for (const auto & address : shard_info.local_addresses)
			{
				BlockInputStreamPtr stream = query_constructor.createLocal(query_ast, new_context, address);
				if (stream)
					res.emplace_back(stream);
			}
		}

		if (create_remote_queries)
		{
			size_t excess = (current_thread < remainder) ? 1 : 0;
			size_t actual_pools_per_thread = pools_per_thread + excess;

			if (actual_pools_per_thread == 1)
			{
				res.emplace_back(query_constructor.createRemote(shard_info.pool, query, new_settings, throttler, context));
				++current_thread;
			}
			else
			{
				if (do_init)
				{
					pools = new ConnectionPools;
					do_init = false;
				}

				pools->push_back(shard_info.pool);
				if (pools->size() == actual_pools_per_thread)
				{
					res.emplace_back(query_constructor.createRemote(pools, query, new_settings, throttler, context));
					do_init = true;
					++current_thread;
				}
			}
		}
	}

	return res;
}

}

}
