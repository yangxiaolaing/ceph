// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <seastar/core/metrics.hh>

#include "crimson/os/seastore/logging.h"

#include "crimson/os/seastore/async_cleaner.h"
#include "crimson/os/seastore/transaction_manager.h"

SET_SUBSYS(seastore_cleaner);

namespace {

enum class gc_formula_t {
  GREEDY,
  BENEFIT,
  COST_BENEFIT,
};
constexpr auto gc_formula = gc_formula_t::COST_BENEFIT;

}

namespace crimson::os::seastore {

void segment_info_t::set_open(
    segment_seq_t _seq, segment_type_t _type,
    data_category_t _category, reclaim_gen_t _generation)
{
  ceph_assert(_seq != NULL_SEG_SEQ);
  ceph_assert(_type != segment_type_t::NULL_SEG);
  ceph_assert(_category != data_category_t::NUM);
  ceph_assert(_generation < RECLAIM_GENERATIONS);
  state = Segment::segment_state_t::OPEN;
  seq = _seq;
  type = _type;
  category = _category;
  generation = _generation;
  written_to = 0;
}

void segment_info_t::set_empty()
{
  state = Segment::segment_state_t::EMPTY;
  seq = NULL_SEG_SEQ;
  type = segment_type_t::NULL_SEG;
  category = data_category_t::NUM;
  generation = NULL_GENERATION;
  modify_time = NULL_TIME;
  num_extents = 0;
  written_to = 0;
}

void segment_info_t::set_closed()
{
  state = Segment::segment_state_t::CLOSED;
  // the rest of information is unchanged
}

void segment_info_t::init_closed(
    segment_seq_t _seq, segment_type_t _type,
    data_category_t _category, reclaim_gen_t _generation,
    std::size_t seg_size)
{
  ceph_assert(_seq != NULL_SEG_SEQ);
  ceph_assert(_type != segment_type_t::NULL_SEG);
  ceph_assert(_category != data_category_t::NUM);
  ceph_assert(_generation < RECLAIM_GENERATIONS);
  state = Segment::segment_state_t::CLOSED;
  seq = _seq;
  type = _type;
  category = _category;
  generation = _generation;
  written_to = seg_size;
}

std::ostream& operator<<(std::ostream &out, const segment_info_t &info)
{
  out << "seg_info_t("
      << "state=" << info.state
      << ", " << info.id;
  if (info.is_empty()) {
    // pass
  } else { // open or closed
    out << " " << info.type
        << " " << segment_seq_printer_t{info.seq}
        << " " << info.category
        << " " << reclaim_gen_printer_t{info.generation}
        << ", modify_time=" << sea_time_point_printer_t{info.modify_time}
        << ", num_extents=" << info.num_extents
        << ", written_to=" << info.written_to;
  }
  return out << ")";
}

void segments_info_t::reset()
{
  segments.clear();

  segment_size = 0;

  journal_segment_id = NULL_SEG_ID;
  num_in_journal_open = 0;
  num_type_journal = 0;
  num_type_ool = 0;

  num_open = 0;
  num_empty = 0;
  num_closed = 0;

  count_open_journal = 0;
  count_open_ool = 0;
  count_release_journal = 0;
  count_release_ool = 0;
  count_close_journal = 0;
  count_close_ool = 0;

  total_bytes = 0;
  avail_bytes_in_open = 0;

  modify_times.clear();
}

void segments_info_t::add_segment_manager(
    SegmentManager &segment_manager)
{
  LOG_PREFIX(segments_info_t::add_segment_manager);
  device_id_t d_id = segment_manager.get_device_id();
  auto ssize = segment_manager.get_segment_size();
  auto nsegments = segment_manager.get_num_segments();
  auto sm_size = segment_manager.get_size();
  INFO("adding segment manager {}, size={}, ssize={}, segments={}",
       device_id_printer_t{d_id}, sm_size, ssize, nsegments);
  ceph_assert(ssize > 0);
  ceph_assert(nsegments > 0);
  ceph_assert(sm_size > 0);

  // also validate if the device is duplicated
  segments.add_device(d_id, nsegments, segment_info_t{});

  // assume all the segment managers share the same settings as follows.
  if (segment_size == 0) {
    ceph_assert(ssize > 0);
    segment_size = ssize;
  } else {
    ceph_assert(segment_size == (std::size_t)ssize);
  }

  // NOTE: by default the segments are empty
  num_empty += nsegments;

  total_bytes += sm_size;
}

void segments_info_t::init_closed(
    segment_id_t segment, segment_seq_t seq, segment_type_t type,
    data_category_t category, reclaim_gen_t generation)
{
  LOG_PREFIX(segments_info_t::init_closed);
  auto& segment_info = segments[segment];
  DEBUG("initiating {} {} {} {} {}, {}, "
        "num_segments(empty={}, opened={}, closed={})",
        segment, type, segment_seq_printer_t{seq},
        category, reclaim_gen_printer_t{generation},
        segment_info, num_empty, num_open, num_closed);
  ceph_assert(segment_info.is_empty());
  ceph_assert(num_empty > 0);
  --num_empty;
  ++num_closed;
  if (type == segment_type_t::JOURNAL) {
    // init_closed won't initialize journal_segment_id
    ceph_assert(get_submitted_journal_head() == JOURNAL_SEQ_NULL);
    ++num_type_journal;
  } else {
    ++num_type_ool;
  }
  // do not increment count_close_*;

  if (segment_info.modify_time != NULL_TIME) {
    modify_times.insert(segment_info.modify_time);
  } else {
    ceph_assert(segment_info.num_extents == 0);
  }

  segment_info.init_closed(
      seq, type, category, generation, get_segment_size());
}

void segments_info_t::mark_open(
    segment_id_t segment, segment_seq_t seq, segment_type_t type,
    data_category_t category, reclaim_gen_t generation)
{
  LOG_PREFIX(segments_info_t::mark_open);
  auto& segment_info = segments[segment];
  INFO("opening {} {} {} {} {}, {}, "
       "num_segments(empty={}, opened={}, closed={})",
       segment, type, segment_seq_printer_t{seq},
       category, reclaim_gen_printer_t{generation},
       segment_info, num_empty, num_open, num_closed);
  ceph_assert(segment_info.is_empty());
  ceph_assert(num_empty > 0);
  --num_empty;
  ++num_open;
  if (type == segment_type_t::JOURNAL) {
    if (journal_segment_id != NULL_SEG_ID) {
      auto& last_journal_segment = segments[journal_segment_id];
      ceph_assert(last_journal_segment.is_closed());
      ceph_assert(last_journal_segment.type == segment_type_t::JOURNAL);
      ceph_assert(last_journal_segment.seq + 1 == seq);
    }
    journal_segment_id = segment;

    ++num_in_journal_open;
    ++num_type_journal;
    ++count_open_journal;
  } else {
    ++num_type_ool;
    ++count_open_ool;
  }
  avail_bytes_in_open += get_segment_size();

  segment_info.set_open(seq, type, category, generation);
}

void segments_info_t::mark_empty(
    segment_id_t segment)
{
  LOG_PREFIX(segments_info_t::mark_empty);
  auto& segment_info = segments[segment];
  INFO("releasing {}, {}, num_segments(empty={}, opened={}, closed={})",
       segment, segment_info,
       num_empty, num_open, num_closed);
  ceph_assert(segment_info.is_closed());
  auto type = segment_info.type;
  assert(type != segment_type_t::NULL_SEG);
  ceph_assert(num_closed > 0);
  --num_closed;
  ++num_empty;
  if (type == segment_type_t::JOURNAL) {
    ceph_assert(num_type_journal > 0);
    --num_type_journal;
    ++count_release_journal;
  } else {
    ceph_assert(num_type_ool > 0);
    --num_type_ool;
    ++count_release_ool;
  }

  if (segment_info.modify_time != NULL_TIME) {
    auto to_erase = modify_times.find(segment_info.modify_time);
    ceph_assert(to_erase != modify_times.end());
    modify_times.erase(to_erase);
  } else {
    ceph_assert(segment_info.num_extents == 0);
  }

  segment_info.set_empty();
}

void segments_info_t::mark_closed(
    segment_id_t segment)
{
  LOG_PREFIX(segments_info_t::mark_closed);
  auto& segment_info = segments[segment];
  INFO("closing {}, {}, num_segments(empty={}, opened={}, closed={})",
       segment, segment_info,
       num_empty, num_open, num_closed);
  ceph_assert(segment_info.is_open());
  ceph_assert(num_open > 0);
  --num_open;
  ++num_closed;
  if (segment_info.type == segment_type_t::JOURNAL) {
    ceph_assert(num_in_journal_open > 0);
    --num_in_journal_open;
    ++count_close_journal;
  } else {
    ++count_close_ool;
  }
  ceph_assert(get_segment_size() >= segment_info.written_to);
  auto seg_avail_bytes = get_segment_size() - segment_info.written_to;
  ceph_assert(avail_bytes_in_open >= seg_avail_bytes);
  avail_bytes_in_open -= seg_avail_bytes;

  if (segment_info.modify_time != NULL_TIME) {
    modify_times.insert(segment_info.modify_time);
  } else {
    ceph_assert(segment_info.num_extents == 0);
  }

  segment_info.set_closed();
}

void segments_info_t::update_written_to(
    segment_type_t type,
    paddr_t offset)
{
  LOG_PREFIX(segments_info_t::update_written_to);
  auto& saddr = offset.as_seg_paddr();
  auto& segment_info = segments[saddr.get_segment_id()];
  if (!segment_info.is_open()) {
    ERROR("segment is not open, not updating, type={}, offset={}, {}",
          type, offset, segment_info);
    ceph_abort();
  }

  auto new_written_to = static_cast<std::size_t>(saddr.get_segment_off());
  ceph_assert(new_written_to <= get_segment_size());
  if (segment_info.written_to > new_written_to) {
    ERROR("written_to should not decrease! type={}, offset={}, {}",
          type, offset, segment_info);
    ceph_abort();
  }

  DEBUG("type={}, offset={}, {}", type, offset, segment_info);
  ceph_assert(type == segment_info.type);
  auto avail_deduction = new_written_to - segment_info.written_to;
  ceph_assert(avail_bytes_in_open >= avail_deduction);
  avail_bytes_in_open -= avail_deduction;
  segment_info.written_to = new_written_to;
}

std::ostream &operator<<(std::ostream &os, const segments_info_t &infos)
{
  return os << "segments("
            << "empty=" << infos.get_num_empty()
            << ", open=" << infos.get_num_open()
            << ", closed=" << infos.get_num_closed()
            << ", type_journal=" << infos.get_num_type_journal()
            << ", type_ool=" << infos.get_num_type_ool()
            << ", total=" << infos.get_total_bytes() << "B"
            << ", available=" << infos.get_available_bytes() << "B"
            << ", unavailable=" << infos.get_unavailable_bytes() << "B"
            << ", available_ratio=" << infos.get_available_ratio()
            << ", submitted_head=" << infos.get_submitted_journal_head()
            << ", time_bound=" << sea_time_point_printer_t{infos.get_time_bound()}
            << ")";
}

bool SpaceTrackerSimple::equals(const SpaceTrackerI &_other) const
{
  LOG_PREFIX(SpaceTrackerSimple::equals);
  const auto &other = static_cast<const SpaceTrackerSimple&>(_other);

  if (other.live_bytes_by_segment.size() != live_bytes_by_segment.size()) {
    ERROR("different segment counts, bug in test");
    assert(0 == "segment counts should match");
    return false;
  }

  bool all_match = true;
  for (auto i = live_bytes_by_segment.begin(), j = other.live_bytes_by_segment.begin();
       i != live_bytes_by_segment.end(); ++i, ++j) {
    if (i->second.live_bytes != j->second.live_bytes) {
      all_match = false;
      DEBUG("segment_id {} live bytes mismatch *this: {}, other: {}",
            i->first, i->second.live_bytes, j->second.live_bytes);
    }
  }
  return all_match;
}

int64_t SpaceTrackerDetailed::SegmentMap::allocate(
  device_segment_id_t segment,
  seastore_off_t offset,
  extent_len_t len,
  const extent_len_t block_size)
{
  LOG_PREFIX(SegmentMap::allocate);
  assert(offset % block_size == 0);
  assert(len % block_size == 0);

  const auto b = (offset / block_size);
  const auto e = (offset + len) / block_size;

  bool error = false;
  for (auto i = b; i < e; ++i) {
    if (bitmap[i]) {
      if (!error) {
        ERROR("found allocated in {}, {} ~ {}", segment, offset, len);
	error = true;
      }
      DEBUG("block {} allocated", i * block_size);
    }
    bitmap[i] = true;
  }
  return update_usage(len);
}

int64_t SpaceTrackerDetailed::SegmentMap::release(
  device_segment_id_t segment,
  seastore_off_t offset,
  extent_len_t len,
  const extent_len_t block_size)
{
  LOG_PREFIX(SegmentMap::release);
  assert(offset % block_size == 0);
  assert(len % block_size == 0);

  const auto b = (offset / block_size);
  const auto e = (offset + len) / block_size;

  bool error = false;
  for (auto i = b; i < e; ++i) {
    if (!bitmap[i]) {
      if (!error) {
	ERROR("found unallocated in {}, {} ~ {}", segment, offset, len);
	error = true;
      }
      DEBUG("block {} unallocated", i * block_size);
    }
    bitmap[i] = false;
  }
  return update_usage(-(int64_t)len);
}

bool SpaceTrackerDetailed::equals(const SpaceTrackerI &_other) const
{
  LOG_PREFIX(SpaceTrackerDetailed::equals);
  const auto &other = static_cast<const SpaceTrackerDetailed&>(_other);

  if (other.segment_usage.size() != segment_usage.size()) {
    ERROR("different segment counts, bug in test");
    assert(0 == "segment counts should match");
    return false;
  }

  bool all_match = true;
  for (auto i = segment_usage.begin(), j = other.segment_usage.begin();
       i != segment_usage.end(); ++i, ++j) {
    if (i->second.get_usage() != j->second.get_usage()) {
      all_match = false;
      ERROR("segment_id {} live bytes mismatch *this: {}, other: {}",
            i->first, i->second.get_usage(), j->second.get_usage());
    }
  }
  return all_match;
}

void SpaceTrackerDetailed::SegmentMap::dump_usage(extent_len_t block_size) const
{
  LOG_PREFIX(SegmentMap::dump_usage);
  INFO("dump start");
  for (unsigned i = 0; i < bitmap.size(); ++i) {
    if (bitmap[i]) {
      LOCAL_LOGGER.info("    {} still live", i * block_size);
    }
  }
}

void SpaceTrackerDetailed::dump_usage(segment_id_t id) const
{
  LOG_PREFIX(SpaceTrackerDetailed::dump_usage);
  INFO("{}", id);
  segment_usage[id].dump_usage(
    block_size_by_segment_manager[id.device_id()]);
}

void SpaceTrackerSimple::dump_usage(segment_id_t id) const
{
  LOG_PREFIX(SpaceTrackerSimple::dump_usage);
  INFO("id: {}, live_bytes: {}",
       id, live_bytes_by_segment[id].live_bytes);
}

AsyncCleaner::AsyncCleaner(
  config_t config,
  SegmentManagerGroupRef&& sm_group,
  BackrefManager &backref_manager,
  bool detailed)
  : detailed(detailed),
    config(config),
    sm_group(std::move(sm_group)),
    backref_manager(backref_manager),
    ool_segment_seq_allocator(
      new SegmentSeqAllocator(segment_type_t::OOL)),
    gc_process(*this)
{
  config.validate();
}

void AsyncCleaner::register_metrics()
{
  namespace sm = seastar::metrics;
  stats.segment_util.buckets.resize(UTIL_BUCKETS);
  std::size_t i;
  for (i = 0; i < UTIL_BUCKETS; ++i) {
    stats.segment_util.buckets[i].upper_bound = ((double)(i + 1)) / 10;
    stats.segment_util.buckets[i].count = 0;
  }
  // NOTE: by default the segments are empty
  i = get_bucket_index(UTIL_STATE_EMPTY);
  stats.segment_util.buckets[i].count = segments.get_num_segments();

  metrics.add_group("async_cleaner", {
    sm::make_counter("segments_number",
		     [this] { return segments.get_num_segments(); },
		     sm::description("the number of segments")),
    sm::make_counter("segment_size",
		     [this] { return segments.get_segment_size(); },
		     sm::description("the bytes of a segment")),
    sm::make_counter("segments_in_journal",
		     [this] { return get_segments_in_journal(); },
		     sm::description("the number of segments in journal")),
    sm::make_counter("segments_type_journal",
		     [this] { return segments.get_num_type_journal(); },
		     sm::description("the number of segments typed journal")),
    sm::make_counter("segments_type_ool",
		     [this] { return segments.get_num_type_ool(); },
		     sm::description("the number of segments typed out-of-line")),
    sm::make_counter("segments_open",
		     [this] { return segments.get_num_open(); },
		     sm::description("the number of open segments")),
    sm::make_counter("segments_empty",
		     [this] { return segments.get_num_empty(); },
		     sm::description("the number of empty segments")),
    sm::make_counter("segments_closed",
		     [this] { return segments.get_num_closed(); },
		     sm::description("the number of closed segments")),

    sm::make_counter("segments_count_open_journal",
		     [this] { return segments.get_count_open_journal(); },
		     sm::description("the count of open journal segment operations")),
    sm::make_counter("segments_count_open_ool",
		     [this] { return segments.get_count_open_ool(); },
		     sm::description("the count of open ool segment operations")),
    sm::make_counter("segments_count_release_journal",
		     [this] { return segments.get_count_release_journal(); },
		     sm::description("the count of release journal segment operations")),
    sm::make_counter("segments_count_release_ool",
		     [this] { return segments.get_count_release_ool(); },
		     sm::description("the count of release ool segment operations")),
    sm::make_counter("segments_count_close_journal",
		     [this] { return segments.get_count_close_journal(); },
		     sm::description("the count of close journal segment operations")),
    sm::make_counter("segments_count_close_ool",
		     [this] { return segments.get_count_close_ool(); },
		     sm::description("the count of close ool segment operations")),

    sm::make_counter("total_bytes",
		     [this] { return segments.get_total_bytes(); },
		     sm::description("the size of the space")),
    sm::make_counter("available_bytes",
		     [this] { return segments.get_available_bytes(); },
		     sm::description("the size of the space is available")),
    sm::make_counter("unavailable_unreclaimable_bytes",
		     [this] { return get_unavailable_unreclaimable_bytes(); },
		     sm::description("the size of the space is unavailable and unreclaimable")),
    sm::make_counter("unavailable_reclaimable_bytes",
		     [this] { return get_unavailable_reclaimable_bytes(); },
		     sm::description("the size of the space is unavailable and reclaimable")),
    sm::make_counter("used_bytes", stats.used_bytes,
		     sm::description("the size of the space occupied by live extents")),
    sm::make_counter("unavailable_unused_bytes",
		     [this] { return get_unavailable_unused_bytes(); },
		     sm::description("the size of the space is unavailable and not alive")),

    sm::make_counter("dirty_journal_bytes",
		     [this] { return get_dirty_journal_size(); },
		     sm::description("the size of the journal for dirty extents")),
    sm::make_counter("alloc_journal_bytes",
		     [this] { return get_alloc_journal_size(); },
		     sm::description("the size of the journal for alloc info")),

    sm::make_counter("projected_count", stats.projected_count,
		    sm::description("the number of projected usage reservations")),
    sm::make_counter("projected_used_bytes_sum", stats.projected_used_bytes_sum,
		    sm::description("the sum of the projected usage in bytes")),

    sm::make_counter("io_count", stats.io_count,
		    sm::description("the sum of IOs")),
    sm::make_counter("io_blocked_count", stats.io_blocked_count,
		    sm::description("IOs that are blocked by gc")),
    sm::make_counter("io_blocked_count_trim", stats.io_blocked_count_trim,
		    sm::description("IOs that are blocked by trimming")),
    sm::make_counter("io_blocked_count_reclaim", stats.io_blocked_count_reclaim,
		    sm::description("IOs that are blocked by reclaimming")),
    sm::make_counter("io_blocked_sum", stats.io_blocked_sum,
		     sm::description("the sum of blocking IOs")),

    sm::make_counter("reclaimed_bytes", stats.reclaimed_bytes,
		     sm::description("rewritten bytes due to reclaim")),
    sm::make_counter("reclaimed_segment_bytes", stats.reclaimed_segment_bytes,
		     sm::description("rewritten bytes due to reclaim")),
    sm::make_counter("closed_journal_used_bytes", stats.closed_journal_used_bytes,
		     sm::description("used bytes when close a journal segment")),
    sm::make_counter("closed_journal_total_bytes", stats.closed_journal_total_bytes,
		     sm::description("total bytes of closed journal segments")),
    sm::make_counter("closed_ool_used_bytes", stats.closed_ool_used_bytes,
		     sm::description("used bytes when close a ool segment")),
    sm::make_counter("closed_ool_total_bytes", stats.closed_ool_total_bytes,
		     sm::description("total bytes of closed ool segments")),

    sm::make_gauge("available_ratio",
                   [this] { return segments.get_available_ratio(); },
                   sm::description("ratio of available space to total space")),
    sm::make_gauge("reclaim_ratio",
                   [this] { return get_reclaim_ratio(); },
                   sm::description("ratio of reclaimable space to unavailable space")),

    sm::make_histogram("segment_utilization_distribution",
		       [this]() -> seastar::metrics::histogram& {
		         return stats.segment_util;
		       },
		       sm::description("utilization distribution of all segments"))
  });
}

segment_id_t AsyncCleaner::allocate_segment(
    segment_seq_t seq,
    segment_type_t type,
    data_category_t category,
    reclaim_gen_t generation)
{
  LOG_PREFIX(AsyncCleaner::allocate_segment);
  assert(seq != NULL_SEG_SEQ);
  for (auto it = segments.begin();
       it != segments.end();
       ++it) {
    auto seg_id = it->first;
    auto& segment_info = it->second;
    if (segment_info.is_empty()) {
      auto old_usage = calc_utilization(seg_id);
      segments.mark_open(seg_id, seq, type, category, generation);
      gc_process.maybe_wake_on_space_used();
      auto new_usage = calc_utilization(seg_id);
      adjust_segment_util(old_usage, new_usage);
      INFO("opened, {}", gc_stat_printer_t{this, false});
      return seg_id;
    }
  }
  ERROR("out of space with {} {} {} {}",
        type, segment_seq_printer_t{seq}, category,
        reclaim_gen_printer_t{generation});
  ceph_abort();
  return NULL_SEG_ID;
}

void AsyncCleaner::update_journal_tails(
  journal_seq_t dirty_tail,
  journal_seq_t alloc_tail)
{
  LOG_PREFIX(AsyncCleaner::update_journal_tails);
  if (disable_trim) return;

  if (dirty_tail != JOURNAL_SEQ_NULL) {
    assert(dirty_tail.offset.get_addr_type() != paddr_types_t::RANDOM_BLOCK);
    ceph_assert(journal_head == JOURNAL_SEQ_NULL ||
                journal_head >= dirty_tail);
    if (journal_dirty_tail != JOURNAL_SEQ_NULL &&
        journal_dirty_tail > dirty_tail) {
      ERROR("journal_dirty_tail {} => {} is backwards!",
            journal_dirty_tail, dirty_tail);
      ceph_abort();
    }
    if (journal_dirty_tail.segment_seq == dirty_tail.segment_seq) {
      DEBUG("journal_dirty_tail {} => {}", journal_dirty_tail, dirty_tail);
    } else {
      INFO("journal_dirty_tail {} => {}", journal_dirty_tail, dirty_tail);
    }
    journal_dirty_tail = dirty_tail;
  }

  if (alloc_tail != JOURNAL_SEQ_NULL) {
    ceph_assert(journal_head == JOURNAL_SEQ_NULL ||
                journal_head >= alloc_tail);
    assert(alloc_tail.offset.get_addr_type() != paddr_types_t::RANDOM_BLOCK);
    if (journal_alloc_tail != JOURNAL_SEQ_NULL &&
        journal_alloc_tail > alloc_tail) {
      ERROR("journal_alloc_tail {} => {} is backwards!",
            journal_alloc_tail, alloc_tail);
      ceph_abort();
    }
    if (journal_alloc_tail.segment_seq == alloc_tail.segment_seq) {
      DEBUG("journal_alloc_tail {} => {}", journal_alloc_tail, alloc_tail);
    } else {
      INFO("journal_alloc_tail {} => {}", journal_alloc_tail, alloc_tail);
    }
    journal_alloc_tail = alloc_tail;
  }

  gc_process.maybe_wake_on_space_used();
  maybe_wake_gc_blocked_io();
}

void AsyncCleaner::close_segment(segment_id_t segment)
{
  LOG_PREFIX(AsyncCleaner::close_segment);
  auto old_usage = calc_utilization(segment);
  segments.mark_closed(segment);
  auto &seg_info = segments[segment];
  if (seg_info.type == segment_type_t::JOURNAL) {
    stats.closed_journal_used_bytes += space_tracker->get_usage(segment);
    stats.closed_journal_total_bytes += segments.get_segment_size();
  } else {
    stats.closed_ool_used_bytes += space_tracker->get_usage(segment);
    stats.closed_ool_total_bytes += segments.get_segment_size();
  }
  auto new_usage = calc_utilization(segment);
  adjust_segment_util(old_usage, new_usage);
  INFO("closed, {} -- {}", gc_stat_printer_t{this, false}, seg_info);
}

AsyncCleaner::trim_alloc_ret AsyncCleaner::trim_alloc(
  Transaction &t,
  journal_seq_t limit)
{
  return backref_manager.merge_cached_backrefs(
    t,
    limit,
    config.rewrite_backref_bytes_per_cycle
  );
}

double AsyncCleaner::calc_gc_benefit_cost(
  segment_id_t id,
  const sea_time_point &now_time,
  const sea_time_point &bound_time) const
{
  double util = calc_utilization(id);
  ceph_assert(util >= 0 && util < 1);
  if constexpr (gc_formula == gc_formula_t::GREEDY) {
    return 1 - util;
  }

  if constexpr (gc_formula == gc_formula_t::COST_BENEFIT) {
    if (util == 0) {
      return std::numeric_limits<double>::max();
    }
    auto modify_time = segments[id].modify_time;
    double age_segment = modify_time.time_since_epoch().count();
    double age_now = now_time.time_since_epoch().count();
    if (likely(age_now > age_segment)) {
      return (1 - util) * (age_now - age_segment) / (2 * util);
    } else {
      // time is wrong
      return (1 - util) / (2 * util);
    }
  }

  assert(gc_formula == gc_formula_t::BENEFIT);
  auto modify_time = segments[id].modify_time;
  double age_factor = 0.5; // middle value if age is invalid
  if (likely(bound_time != NULL_TIME &&
             modify_time != NULL_TIME &&
             now_time > modify_time)) {
    assert(modify_time >= bound_time);
    double age_bound = bound_time.time_since_epoch().count();
    double age_now = now_time.time_since_epoch().count();
    double age_segment = modify_time.time_since_epoch().count();
    age_factor = (age_now - age_segment) / (age_now - age_bound);
  }
  return ((1 - 2 * age_factor) * util * util +
          (2 * age_factor - 2) * util + 1);
}

AsyncCleaner::rewrite_dirty_ret AsyncCleaner::rewrite_dirty(
  Transaction &t,
  journal_seq_t limit)
{
  return ecb->get_next_dirty_extents(
    t,
    limit,
    config.rewrite_dirty_bytes_per_cycle
  ).si_then([=, &t, this](auto dirty_list) {
    LOG_PREFIX(AsyncCleaner::rewrite_dirty);
    DEBUGT("rewrite {} dirty extents", t, dirty_list.size());
    return seastar::do_with(
      std::move(dirty_list),
      [this, FNAME, &t](auto &dirty_list) {
	return trans_intr::do_for_each(
	  dirty_list,
	  [this, FNAME, &t](auto &e) {
	  DEBUGT("cleaning {}", t, *e);
	  return ecb->rewrite_extent(t, e, DIRTY_GENERATION, NULL_TIME);
	});
      });
  });
}

AsyncCleaner::gc_cycle_ret AsyncCleaner::GCProcess::run()
{
  return seastar::do_until(
    [this] { return is_stopping(); },
    [this] {
      return maybe_wait_should_run(
      ).then([this] {
	cleaner.log_gc_state("GCProcess::run");

	if (is_stopping()) {
	  return seastar::now();
	} else {
	  return cleaner.do_gc_cycle();
	}
      });
    });
}

AsyncCleaner::gc_cycle_ret AsyncCleaner::do_gc_cycle()
{
  if (gc_should_trim_alloc()) {
    return gc_trim_alloc(
    ).handle_error(
      crimson::ct_error::assert_all{
	"GCProcess::run encountered invalid error in gc_trim_alloc"
      }
    );
  } else if (gc_should_trim_dirty()) {
    return gc_trim_dirty(
    ).handle_error(
      crimson::ct_error::assert_all{
	"GCProcess::run encountered invalid error in gc_trim_dirty"
      }
    );
  } else if (gc_should_reclaim_space()) {
    return gc_reclaim_space(
    ).handle_error(
      crimson::ct_error::assert_all{
	"GCProcess::run encountered invalid error in gc_reclaim_space"
      }
    );
  } else {
    return seastar::now();
  }
}

AsyncCleaner::gc_trim_alloc_ret
AsyncCleaner::gc_trim_alloc() {
  return repeat_eagain([this] {
    return ecb->with_transaction_intr(
      Transaction::src_t::CLEANER_TRIM_ALLOC,
      "trim_alloc",
      [this](auto &t)
    {
      LOG_PREFIX(AsyncCleaner::gc_trim_alloc);
      DEBUGT("target {}", t, get_alloc_tail_target());
      return trim_alloc(t, get_alloc_tail_target()
      ).si_then([this, &t](auto trim_alloc_to)
        -> ExtentCallbackInterface::submit_transaction_direct_iertr::future<>
      {
        if (trim_alloc_to != JOURNAL_SEQ_NULL) {
          return ecb->submit_transaction_direct(
            t, std::make_optional<journal_seq_t>(trim_alloc_to));
        }
        return seastar::now();
      });
    });
  });
}

AsyncCleaner::gc_trim_dirty_ret AsyncCleaner::gc_trim_dirty()
{
  return repeat_eagain([this] {
    return ecb->with_transaction_intr(
      Transaction::src_t::CLEANER_TRIM_DIRTY,
      "trim_dirty",
      [this](auto &t)
    {
      return rewrite_dirty(t, get_dirty_tail_target()
      ).si_then([this, &t] {
        return ecb->submit_transaction_direct(t);
      });
    });
  });
}

AsyncCleaner::retrieve_live_extents_ret
AsyncCleaner::_retrieve_live_extents(
  Transaction &t,
  std::set<
    backref_entry_t,
    backref_entry_t::cmp_t> &&backrefs,
  std::vector<CachedExtentRef> &extents)
{
  return seastar::do_with(
    std::move(backrefs),
    [this, &t, &extents](auto &backrefs) {
    return trans_intr::parallel_for_each(
      backrefs,
      [this, &extents, &t](auto &ent) {
      LOG_PREFIX(AsyncCleaner::_retrieve_live_extents);
      DEBUGT("getting extent of type {} at {}~{}",
	t,
	ent.type,
	ent.paddr,
	ent.len);
      return ecb->get_extents_if_live(
	t, ent.type, ent.paddr, ent.laddr, ent.len
      ).si_then([&extents, &ent, &t](auto list) {
	LOG_PREFIX(AsyncCleaner::_retrieve_live_extents);
	if (list.empty()) {
	  DEBUGT("addr {} dead, skipping", t, ent.paddr);
	} else {
	  for (auto &e : list) {
	    extents.emplace_back(std::move(e));
	  }
	}
	return ExtentCallbackInterface::rewrite_extent_iertr::now();
      });
    });
  });
}

AsyncCleaner::retrieve_backref_mappings_ret
AsyncCleaner::retrieve_backref_mappings(
  paddr_t start_paddr,
  paddr_t end_paddr)
{
  return seastar::do_with(
    backref_pin_list_t(),
    [this, start_paddr, end_paddr](auto &pin_list) {
    return repeat_eagain([this, start_paddr, end_paddr, &pin_list] {
      return ecb->with_transaction_intr(
	Transaction::src_t::READ,
	"get_backref_mappings",
	[this, start_paddr, end_paddr](auto &t) {
	return backref_manager.get_mappings(
	  t, start_paddr, end_paddr
	);
      }).safe_then([&pin_list](auto&& list) {
	pin_list = std::move(list);
      });
    }).safe_then([&pin_list] {
      return seastar::make_ready_future<backref_pin_list_t>(std::move(pin_list));
    });
  });
}

AsyncCleaner::gc_reclaim_space_ret AsyncCleaner::gc_reclaim_space()
{
  LOG_PREFIX(AsyncCleaner::gc_reclaim_space);
  if (!reclaim_state) {
    segment_id_t seg_id = get_next_reclaim_segment();
    auto &segment_info = segments[seg_id];
    INFO("reclaim {} {} start, usage={}, time_bound={}",
         seg_id, segment_info,
         space_tracker->calc_utilization(seg_id),
         sea_time_point_printer_t{segments.get_time_bound()});
    ceph_assert(segment_info.is_closed());
    reclaim_state = reclaim_state_t::create(
        seg_id, segment_info.generation, segments.get_segment_size());
  }
  reclaim_state->advance(config.reclaim_bytes_per_cycle);

  DEBUG("reclaiming {} {}~{}",
        reclaim_gen_printer_t{reclaim_state->generation},
        reclaim_state->start_pos,
        reclaim_state->end_pos);
  double pavail_ratio = get_projected_available_ratio();
  sea_time_point start = seastar::lowres_system_clock::now();

  return seastar::do_with(
    (size_t)0,
    (size_t)0,
    [this, pavail_ratio, start](
      auto &reclaimed,
      auto &runs) {
    return retrieve_backref_mappings(
      reclaim_state->start_pos,
      reclaim_state->end_pos
    ).safe_then([this, &reclaimed, &runs](auto pin_list) {
      return seastar::do_with(
	std::move(pin_list),
	[this, &reclaimed, &runs](auto &pin_list) {
	return repeat_eagain(
	  [this, &reclaimed, &runs, &pin_list]() mutable {
	  reclaimed = 0;
	  runs++;
	  return ecb->with_transaction_intr(
	    Transaction::src_t::CLEANER_RECLAIM,
	    "reclaim_space",
	    [this, &reclaimed, &pin_list](auto &t) {
	    return seastar::do_with(
	      std::vector<CachedExtentRef>(),
	      [this, &reclaimed, &t, &pin_list]
	      (auto &extents) {
	      return backref_manager.retrieve_backref_extents(
		t,
		backref_manager.get_cached_backref_extents_in_range(
		  reclaim_state->start_pos, reclaim_state->end_pos),
		extents
	      ).si_then([this, &extents, &t, &pin_list] {
		// calculate live extents
		auto cached_backrefs = 
		  backref_manager.get_cached_backref_entries_in_range(
		    reclaim_state->start_pos, reclaim_state->end_pos);
		std::set<
		  backref_entry_t,
		  backref_entry_t::cmp_t> backrefs;
		for (auto &pin : pin_list) {
		  backrefs.emplace(pin->get_key(), pin->get_val(),
		    pin->get_length(), pin->get_type(), journal_seq_t());
		}
		for (auto &backref : cached_backrefs) {
		  if (backref.laddr == L_ADDR_NULL) {
		    auto it = backrefs.find(backref.paddr);
		    assert(it->len == backref.len);
		    backrefs.erase(it);
		  } else {
		    backrefs.emplace(backref.paddr, backref.laddr,
		      backref.len, backref.type, backref.seq);
		  }
		}
		return _retrieve_live_extents(
		  t, std::move(backrefs), extents);
	      }).si_then([&extents, this, &t, &reclaimed] {
		auto modify_time = segments[reclaim_state->get_segment_id()].modify_time;
		return trans_intr::do_for_each(
		  extents,
		  [this, modify_time, &t, &reclaimed](auto &ext) {
		  reclaimed += ext->get_length();
		  return ecb->rewrite_extent(
		      t, ext, reclaim_state->target_generation, modify_time);
		});
	      });
	    }).si_then([this, &t] {
	      if (reclaim_state->is_complete()) {
		t.mark_segment_to_release(reclaim_state->get_segment_id());
	      }
	      return ecb->submit_transaction_direct(t);
	    });
	  });
	});
      });
    }).safe_then(
      [&reclaimed, this, pavail_ratio, start, &runs] {
      LOG_PREFIX(AsyncCleaner::gc_reclaim_space);
      stats.reclaiming_bytes += reclaimed;
      auto d = seastar::lowres_system_clock::now() - start;
      DEBUG("duration: {}, pavail_ratio before: {}, repeats: {}", d, pavail_ratio, runs);
      if (reclaim_state->is_complete()) {
	INFO("reclaim {} finish, reclaimed alive/total={}, usage={}",
             reclaim_state->get_segment_id(),
             stats.reclaiming_bytes/(double)segments.get_segment_size(),
             space_tracker->calc_utilization(reclaim_state->get_segment_id()));
	stats.reclaimed_bytes += stats.reclaiming_bytes;
	stats.reclaimed_segment_bytes += segments.get_segment_size();
	stats.reclaiming_bytes = 0;
	reclaim_state.reset();
      }
    });
  });
}

AsyncCleaner::mount_ret AsyncCleaner::mount()
{
  LOG_PREFIX(AsyncCleaner::mount);
  const auto& sms = sm_group->get_segment_managers();
  INFO("{} segment managers", sms.size());
  init_complete = false;
  stats = {};
  journal_head = JOURNAL_SEQ_NULL;
  journal_alloc_tail = JOURNAL_SEQ_NULL;
  journal_dirty_tail = JOURNAL_SEQ_NULL;
  
  space_tracker.reset(
    detailed ?
    (SpaceTrackerI*)new SpaceTrackerDetailed(
      sms) :
    (SpaceTrackerI*)new SpaceTrackerSimple(
      sms));
  
  segments.reset();
  for (auto sm : sms) {
    segments.add_segment_manager(*sm);
  }
  segments.assign_ids();
  metrics.clear();
  register_metrics();

  INFO("{} segments", segments.get_num_segments());
  return crimson::do_for_each(
    segments.begin(),
    segments.end(),
    [this, FNAME](auto& it)
  {
    auto segment_id = it.first;
    return sm_group->read_segment_header(
      segment_id
    ).safe_then([segment_id, this, FNAME](auto header) {
      DEBUG("segment_id={} -- {}", segment_id, header);
      auto s_type = header.get_type();
      if (s_type == segment_type_t::NULL_SEG) {
        ERROR("got null segment, segment_id={} -- {}", segment_id, header);
        ceph_abort();
      }
      return sm_group->read_segment_tail(
        segment_id
      ).safe_then([this, FNAME, segment_id, header](auto tail)
        -> scan_extents_ertr::future<> {
        if (tail.segment_nonce != header.segment_nonce) {
          return scan_no_tail_segment(header, segment_id);
        }

        sea_time_point modify_time = mod_to_timepoint(tail.modify_time);
        std::size_t num_extents = tail.num_extents;
        if ((modify_time == NULL_TIME && num_extents == 0) ||
            (modify_time != NULL_TIME && num_extents != 0)) {
          segments.update_modify_time(segment_id, modify_time, num_extents);
        } else {
          ERROR("illegal modify time {}", tail);
          return crimson::ct_error::input_output_error::make();
        }

        init_mark_segment_closed(
          segment_id,
          header.segment_seq,
          header.type,
          header.category,
          header.generation);
        return seastar::now();
      }).handle_error(
        crimson::ct_error::enodata::handle(
          [this, header, segment_id](auto) {
          return scan_no_tail_segment(header, segment_id);
        }),
        crimson::ct_error::pass_further_all{}
      );
    }).handle_error(
      crimson::ct_error::enoent::handle([](auto) {
        return mount_ertr::now();
      }),
      crimson::ct_error::enodata::handle([](auto) {
        return mount_ertr::now();
      }),
      crimson::ct_error::input_output_error::pass_further{},
      crimson::ct_error::assert_all{"unexpected error"}
    );
  }).safe_then([this, FNAME] {
    INFO("done, {}", segments);
  });
}

AsyncCleaner::scan_extents_ret AsyncCleaner::scan_no_tail_segment(
  const segment_header_t &segment_header,
  segment_id_t segment_id)
{
  LOG_PREFIX(AsyncCleaner::scan_no_tail_segment);
  INFO("scan {} {}", segment_id, segment_header);
  return seastar::do_with(
    scan_valid_records_cursor({
      segments[segment_id].seq,
      paddr_t::make_seg_paddr(segment_id, 0)
    }),
    SegmentManagerGroup::found_record_handler_t(
      [this, segment_id, segment_header, FNAME](
        record_locator_t locator,
        const record_group_header_t &record_group_header,
        const bufferlist& mdbuf
      ) mutable -> SegmentManagerGroup::scan_valid_records_ertr::future<>
    {
      DEBUG("{} {}, decoding {} records",
            segment_id, segment_header.get_type(), record_group_header.records);

      auto maybe_headers = try_decode_record_headers(
          record_group_header, mdbuf);
      if (!maybe_headers) {
        // This should be impossible, we did check the crc on the mdbuf
        ERROR("unable to decode record headers for record group {}",
          locator.record_block_base);
        return crimson::ct_error::input_output_error::make();
      }

      for (auto &record_header : *maybe_headers) {
        auto modify_time = mod_to_timepoint(record_header.modify_time);
        if (record_header.extents == 0 || modify_time != NULL_TIME) {
          segments.update_modify_time(
              segment_id, modify_time, record_header.extents);
        } else {
          ERROR("illegal modify time {}", record_header);
          return crimson::ct_error::input_output_error::make();
        }
      }
      return seastar::now();
    }),
    [this, segment_header](auto &cursor, auto &handler)
  {
    return sm_group->scan_valid_records(
      cursor,
      segment_header.segment_nonce,
      segments.get_segment_size(),
      handler).discard_result();
  }).safe_then([this, segment_id, segment_header] {
    init_mark_segment_closed(
      segment_id,
      segment_header.segment_seq,
      segment_header.type,
      segment_header.category,
      segment_header.generation);
  });
}

AsyncCleaner::release_ertr::future<>
AsyncCleaner::maybe_release_segment(Transaction &t)
{
  auto to_release = t.get_segment_to_release();
  if (to_release != NULL_SEG_ID) {
    LOG_PREFIX(AsyncCleaner::maybe_release_segment);
    INFOT("releasing segment {}", t, to_release);
    return sm_group->release_segment(to_release
    ).safe_then([this, FNAME, &t, to_release] {
      auto old_usage = calc_utilization(to_release);
      if(unlikely(old_usage != 0)) {
	space_tracker->dump_usage(to_release);
	ERRORT("segment {} old_usage {} != 0", t, to_release, old_usage);
	ceph_abort();
      }
      segments.mark_empty(to_release);
      auto new_usage = calc_utilization(to_release);
      adjust_segment_util(old_usage, new_usage);
      INFOT("released, {}", t, gc_stat_printer_t{this, false});
      if (space_tracker->get_usage(to_release) != 0) {
        space_tracker->dump_usage(to_release);
        ceph_abort();
      }
      maybe_wake_gc_blocked_io();
    });
  } else {
    return SegmentManager::release_ertr::now();
  }
}

void AsyncCleaner::complete_init()
{
  LOG_PREFIX(AsyncCleaner::complete_init);
  if (disable_trim) {
    init_complete = true;
    return;
  }
  init_complete = true;
  INFO("done, start GC, {}", gc_stat_printer_t{this, true});
  ceph_assert(journal_head != JOURNAL_SEQ_NULL);
  ceph_assert(journal_alloc_tail != JOURNAL_SEQ_NULL);
  ceph_assert(journal_dirty_tail != JOURNAL_SEQ_NULL);
  gc_process.start();
}

seastar::future<> AsyncCleaner::stop()
{
  return gc_process.stop(
  ).then([this] {
    LOG_PREFIX(AsyncCleaner::stop);
    INFO("done, {}", gc_stat_printer_t{this, true});
  });
}

void AsyncCleaner::mark_space_used(
  paddr_t addr,
  extent_len_t len,
  bool init_scan)
{
  LOG_PREFIX(AsyncCleaner::mark_space_used);
  if (addr.get_addr_type() != paddr_types_t::SEGMENT) {
    return;
  }
  auto& seg_addr = addr.as_seg_paddr();

  if (!init_scan && !init_complete) {
    return;
  }

  stats.used_bytes += len;
  auto old_usage = calc_utilization(seg_addr.get_segment_id());
  [[maybe_unused]] auto ret = space_tracker->allocate(
    seg_addr.get_segment_id(),
    seg_addr.get_segment_off(),
    len);
  auto new_usage = calc_utilization(seg_addr.get_segment_id());
  adjust_segment_util(old_usage, new_usage);

  gc_process.maybe_wake_on_space_used();
  assert(ret > 0);
  DEBUG("segment {} new len: {}~{}, live_bytes: {}",
        seg_addr.get_segment_id(),
        addr,
        len,
        space_tracker->get_usage(seg_addr.get_segment_id()));
}

void AsyncCleaner::mark_space_free(
  paddr_t addr,
  extent_len_t len,
  bool init_scan)
{
  LOG_PREFIX(AsyncCleaner::mark_space_free);
  if (!init_complete && !init_scan) {
    return;
  }
  if (addr.get_addr_type() != paddr_types_t::SEGMENT) {
    return;
  }

  ceph_assert(stats.used_bytes >= len);
  stats.used_bytes -= len;
  auto& seg_addr = addr.as_seg_paddr();

  DEBUG("segment {} free len: {}~{}",
        seg_addr.get_segment_id(), addr, len);
  auto old_usage = calc_utilization(seg_addr.get_segment_id());
  [[maybe_unused]] auto ret = space_tracker->release(
    seg_addr.get_segment_id(),
    seg_addr.get_segment_off(),
    len);
  auto new_usage = calc_utilization(seg_addr.get_segment_id());
  adjust_segment_util(old_usage, new_usage);
  maybe_wake_gc_blocked_io();
  assert(ret >= 0);
  DEBUG("segment {} free len: {}~{}, live_bytes: {}",
        seg_addr.get_segment_id(),
        addr,
        len,
        space_tracker->get_usage(seg_addr.get_segment_id()));
}

segment_id_t AsyncCleaner::get_next_reclaim_segment() const
{
  LOG_PREFIX(AsyncCleaner::get_next_reclaim_segment);
  segment_id_t id = NULL_SEG_ID;
  double max_benefit_cost = 0;
  sea_time_point now_time;
  if constexpr (gc_formula != gc_formula_t::GREEDY) {
    now_time = seastar::lowres_system_clock::now();
  } else {
    now_time = NULL_TIME;
  }
  sea_time_point bound_time;
  if constexpr (gc_formula == gc_formula_t::BENEFIT) {
    bound_time = segments.get_time_bound();
    if (bound_time == NULL_TIME) {
      WARN("BENEFIT -- bound_time is NULL_TIME");
    }
  } else {
    bound_time = NULL_TIME;
  }
  for (auto& [_id, segment_info] : segments) {
    if (segment_info.is_closed() &&
        !segment_info.is_in_journal(get_journal_tail())) {
      double benefit_cost = calc_gc_benefit_cost(_id, now_time, bound_time);
      if (benefit_cost > max_benefit_cost) {
        id = _id;
        max_benefit_cost = benefit_cost;
      }
    }
  }
  if (id != NULL_SEG_ID) {
    DEBUG("segment {}, benefit_cost {}",
          id, max_benefit_cost);
    return id;
  } else {
    ceph_assert(get_segments_reclaimable() == 0);
    // see gc_should_reclaim_space()
    ceph_abort("impossible!");
    return NULL_SEG_ID;
  }
}

void AsyncCleaner::log_gc_state(const char *caller) const
{
  LOG_PREFIX(AsyncCleaner::log_gc_state);
  if (LOCAL_LOGGER.is_enabled(seastar::log_level::debug) &&
      !disable_trim) {
    DEBUG("caller {}, {}", caller, gc_stat_printer_t{this, true});
  }
}

seastar::future<>
AsyncCleaner::reserve_projected_usage(std::size_t projected_usage)
{
  if (disable_trim) {
    return seastar::now();
  }
  ceph_assert(init_complete);
  // The pipeline configuration prevents another IO from entering
  // prepare until the prior one exits and clears this.
  ceph_assert(!blocked_io_wake);
  ++stats.io_count;
  bool is_blocked = false;
  if (should_block_on_trim()) {
    is_blocked = true;
    ++stats.io_blocked_count_trim;
  }
  if (should_block_on_reclaim()) {
    is_blocked = true;
    ++stats.io_blocked_count_reclaim;
  }
  if (is_blocked) {
    ++stats.io_blocking_num;
    ++stats.io_blocked_count;
    stats.io_blocked_sum += stats.io_blocking_num;
  }
  return seastar::do_until(
    [this] {
      log_gc_state("await_hard_limits");
      return !should_block_on_gc();
    },
    [this] {
      blocked_io_wake = seastar::promise<>();
      return blocked_io_wake->get_future();
    }
  ).then([this, projected_usage, is_blocked] {
    ceph_assert(!blocked_io_wake);
    stats.projected_used_bytes += projected_usage;
    ++stats.projected_count;
    stats.projected_used_bytes_sum += stats.projected_used_bytes;
    if (is_blocked) {
      assert(stats.io_blocking_num > 0);
      --stats.io_blocking_num;
    }
  });
}

void AsyncCleaner::release_projected_usage(std::size_t projected_usage)
{
  if (disable_trim) return;
  ceph_assert(init_complete);
  ceph_assert(stats.projected_used_bytes >= projected_usage);
  stats.projected_used_bytes -= projected_usage;
  return maybe_wake_gc_blocked_io();
}

std::ostream &operator<<(std::ostream &os, AsyncCleaner::gc_stat_printer_t stats)
{
  os << "gc_stats(";
  if (stats.cleaner->init_complete) {
    os << "should_block_on_(trim=" << stats.cleaner->should_block_on_trim()
       << ", reclaim=" << stats.cleaner->should_block_on_reclaim() << ")"
       << ", should_(trim_dirty=" << stats.cleaner->gc_should_trim_dirty()
       << ", trim_alloc=" << stats.cleaner->gc_should_trim_alloc()
       << ", reclaim=" << stats.cleaner->gc_should_reclaim_space() << ")";
  } else {
    os << "init";
  }
  os << ", projected_avail_ratio=" << stats.cleaner->get_projected_available_ratio()
     << ", reclaim_ratio=" << stats.cleaner->get_reclaim_ratio()
     << ", alive_ratio=" << stats.cleaner->get_alive_ratio();
  if (stats.detailed) {
    os << ", journal_head=" << stats.cleaner->journal_head
       << ", alloc_tail=" << stats.cleaner->journal_alloc_tail
       << ", dirty_tail=" << stats.cleaner->journal_dirty_tail;
    if (stats.cleaner->init_complete) {
      os << ", alloc_tail_target=" << stats.cleaner->get_alloc_tail_target()
         << ", dirty_tail_target=" << stats.cleaner->get_dirty_tail_target()
         << ", tail_limit=" << stats.cleaner->get_tail_limit();
    }
    os << ", unavailable_unreclaimable="
       << stats.cleaner->get_unavailable_unreclaimable_bytes() << "B"
       << ", unavailable_reclaimble="
       << stats.cleaner->get_unavailable_reclaimable_bytes() << "B"
       << ", alive=" << stats.cleaner->stats.used_bytes << "B";
  }
  os << ")";
  if (stats.detailed) {
    os << ", " << stats.cleaner->segments;
  }
  return os;
}

}
