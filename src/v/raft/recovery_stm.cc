#include "raft/recovery_stm.h"

#include "outcome_future_utils.h"
#include "raft/consensus_utils.h"
#include "raft/errc.h"
#include "raft/logger.h"
#include "raft/raftgen_service.h"

#include <seastar/core/future-util.hh>

#include <chrono>

namespace raft {
using namespace std::chrono_literals;

recovery_stm::recovery_stm(
  consensus* p, follower_index_metadata& meta, ss::io_priority_class prio)
  : _ptr(p)
  , _meta(meta)
  , _prio(prio)
  , _ctxlog(_ptr->_self, raft::group_id(_ptr->_meta.group)) {}

ss::future<> recovery_stm::do_one_read() {
    storage::log_reader_config cfg{
      .start_offset = _meta.next_index, // next entry
      .max_bytes = 1024 * 1024,         // 1MB
      .min_bytes = 1,                   // we know at least 1 entry
      .prio = _prio,
      .type_filter = {},
      // We have to send all the records that leader have, event those that are
      // beyond commit index, thanks to that after majority have recovered
      // leader can update its commit index
      .max_offset = model::offset(_ptr->_log.max_offset()) // inclusive
    };

    return ss::do_with(
             _ptr->_log.make_reader(cfg),
             [this](model::record_batch_reader& reader) {
                 return reader.consume(
                   details::memory_batch_consumer(), model::no_timeout);
             })
      .then([this](std::vector<model::record_batch> batches) {
          // wrap in a foreign core destructor
          _ctxlog.trace(
            "Read {} batches for {} node recovery",
            batches.size(),
            _meta.node_id);
          _base_batch_offset = batches.begin()->base_offset();
          _last_batch_offset = batches.back().last_offset();
          return details::foreign_share_n(
            model::make_memory_record_batch_reader(std::move(batches)), 1);
      })
      .then([this](std::vector<model::record_batch_reader> readers) {
          return replicate(std::move(readers.back()));
      });
}

ss::future<> recovery_stm::replicate(model::record_batch_reader&& reader) {
    // collect metadata for append entries request
    // last persisted offset is last_offset of batch before the first one in the
    // reader
    auto prev_log_idx = details::prev_offset(_base_batch_offset);
    // get term for prev_log_idx batch
    auto prev_log_term = _ptr->get_term(prev_log_idx);
    // calculate commit index for follower to update immediately
    auto commit_idx = std::min(
      _last_batch_offset(),
      static_cast<model::offset::type>(_ptr->_meta.commit_index));
    // build request
    auto r = append_entries_request{
      .node_id = _meta.node_id,
      .meta = protocol_metadata{.group = _ptr->_meta.group,
                                .commit_index = commit_idx,
                                .term = _ptr->_meta.term,
                                .prev_log_index = prev_log_idx(),
                                .prev_log_term = prev_log_term()},
      .batches = std::move(reader)};

    _ptr->update_node_hbeat_timestamp(_meta.node_id);

    return dispatch_append_entries(std::move(r)).then([this](auto r) {
        if (!r) {
            _ctxlog.error(
              "recovery_stm: not replicate entry: {} - {}",
              r,
              r.error().message());
            _stop_requested = true;
        }

        return _ptr->process_append_reply(_meta.node_id, r.value())
          .then([v = r.value(), this] {
              // move the follower next index backward if recovery were not
              // successfull
              //
              // Raft paper:
              // If AppendEntries fails because of log inconsistency: decrement
              // nextIndex and retry(§5.3)

              if (!v.success) {
                  _meta.next_index = std::max(
                    model::offset(0), details::prev_offset(_base_batch_offset));
                  _ctxlog.trace(
                    "Move node {} next index {} backward",
                    _meta.node_id,
                    _meta.next_index);
              }
          });
    });
}

ss::future<result<append_entries_reply>>
recovery_stm::dispatch_append_entries(append_entries_request&& r) {
    using ret_t = result<append_entries_reply>;
    auto shard = rpc::connection_cache::shard_for(_meta.node_id);
    return ss::smp::submit_to(shard, [this, r = std::move(r)]() mutable {
        auto& local = _ptr->_clients.local();
        if (!local.contains(_meta.node_id)) {
            return ss::make_ready_future<ret_t>(errc::missing_tcp_client);
        }
        return local.get(_meta.node_id)
          ->get_connected()
          .then([r = std::move(r)](result<rpc::transport*> cli) mutable {
              if (!cli) {
                  return ss::make_ready_future<ret_t>(cli.error());
              }
              // TODO: Make timeout configurable
              auto f = raftgen_client_protocol(*cli.value())
                         .append_entries(
                           std::move(r), raft::clock_type::now() + 1s);
              using rpc_ret_t
                = result<rpc::client_context<append_entries_reply>, raft::errc>;
              return wrap_exception_with_result<rpc::request_timeout_exception>(
                       errc::timeout, std::move(f))
                .then([](rpc_ret_t r) {
                    if (!r) {
                        return ss::make_ready_future<ret_t>(r.error());
                    }
                    return ss::make_ready_future<ret_t>(r.value().data);
                });
          });
    });
}

bool recovery_stm::is_recovery_finished() {
    return _meta.match_index == _ptr->_log.max_offset() // fully caught up
           || _stop_requested                           // stop requested
           || _ptr->_vstate
                != consensus::vote_state::leader; // not a leader anymore
}

ss::future<> recovery_stm::apply() {
    return ss::do_until(
             [this] { return is_recovery_finished(); },
             [this] { return do_one_read(); })
      .finally([this] {
          _ctxlog.trace("Finished node {} recovery", _meta.node_id);
          _meta.is_recovering = false;
      });
}
} // namespace raft