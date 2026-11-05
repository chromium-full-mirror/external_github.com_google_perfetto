/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/plugins/wattson/wattson_soc_model_module.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_wattson_soc_model.pbzero.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/global_metadata_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/track_compressor.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/plugins/wattson/tables_py.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/trace_processor_context_ptr.h"
#include "src/trace_processor/util/clock_synchronizer.h"
#include "src/trace_processor/util/descriptors.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::wattson {
namespace {

using ::com::android::internal::pbzero::FrameworksBaseTracePacket;
using ::com::android::internal::pbzero::WattsonCpuCluster;
using ::com::android::internal::pbzero::WattsonCurve;
using ::com::android::internal::pbzero::WattsonIdleStateMap;
using ::com::android::internal::pbzero::WattsonSocModel;
using ::com::android::internal::pbzero::WattsonSubsystem;
using ::testing::DoubleEq;

class WattsonSocModelModuleTest : public testing::Test {
 public:
  WattsonSocModelModuleTest() {
    context_.storage = std::make_unique<TraceStorage>();
    storage_ = context_.storage.get();
    tables_ = std::make_unique<WattsonExternalTables>(
        storage_->mutable_string_pool());
    context_.register_additional_proto_modules =
        [this](ProtoImporterModuleContext* module_context,
               TraceProcessorContext* context) {
          module_context->modules.emplace_back(
              std::make_unique<WattsonSocModelModule>(module_context, context,
                                                      tables_.get()));
        };
    context_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(storage_);
    context_.machine_tracker =
        std::make_unique<MachineTracker>(&context_, kDefaultMachineId);
    context_.global_metadata_tracker =
        std::make_unique<GlobalMetadataTracker>(storage_);
    context_.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId(0)});
    context_.stats_tracker = std::make_unique<StatsTracker>(&context_);
    context_.metadata_tracker = std::make_unique<MetadataTracker>(&context_);
    context_.import_logs_tracker =
        std::make_unique<ImportLogsTracker>(&context_, TraceId(1));
    context_.trace_time_state = std::make_unique<TraceTimeState>(
        ClockId::Machine(protos::pbzero::BUILTIN_CLOCK_BOOTTIME));
    primary_sync_ = std::make_unique<ClockSynchronizer>(
        context_.trace_time_state.get(),
        std::make_unique<ClockSynchronizerListenerImpl>(&context_));
    context_.clock_tracker = std::make_unique<ClockTracker>(
        &context_, primary_sync_.get(), /*is_primary=*/true);
    context_.track_tracker = std::make_unique<TrackTracker>(&context_);
    context_.slice_tracker = std::make_unique<SliceTracker>(&context_);
    context_.args_tracker = std::make_unique<ArgsTracker>(storage_);
    context_.slice_translation_table =
        std::make_unique<SliceTranslationTable>(storage_);
    context_.process_track_translation_table =
        std::make_unique<ProcessTrackTranslationTable>(storage_);
    context_.args_translation_table =
        std::make_unique<ArgsTranslationTable>(storage_);
    context_.track_compressor = std::make_unique<TrackCompressor>(&context_);
    context_.descriptor_pool_ = std::make_unique<DescriptorPool>();

    context_.sorter = std::make_unique<TraceSorter>(
        &context_, TraceSorter::SortingMode::kFullSort);
    reader_ = std::make_unique<ProtoTraceReader>(&context_);
  }

  void Tokenize(const uint8_t* data, size_t size) {
    auto blob = TraceBlob::CopyFrom(data, size);
    auto status = reader_->Parse(TraceBlobView(std::move(blob)));
    ASSERT_TRUE(status.ok()) << status.message();
    context_.sorter->ExtractEventsForced();
  }

 protected:
  TraceProcessorContext context_;
  TraceStorage* storage_;
  std::unique_ptr<WattsonExternalTables> tables_;
  std::unique_ptr<ClockSynchronizer> primary_sync_;
  std::unique_ptr<ProtoTraceReader> reader_;
};

TEST_F(WattsonSocModelModuleTest, ParseWattsonSocModel) {
  protozero::HeapBuffered<protos::pbzero::Trace> trace;

  // Build a WattsonSocModel
  protozero::HeapBuffered<WattsonSocModel> model;
  model->set_device_name("Tensor G6");
  model->set_model_version(1);
  model->set_model_source(WattsonSocModel::MODEL_SOURCE_VENDOR);
  model->set_use_devfreq(true);
  model->set_gpu_id(0);

  // CPU clusters: CPU 0..3 -> cluster 0, CPU 4..6 -> cluster 4,
  // CPU 7 -> cluster 7
  {
    auto* cl = model->add_cpu_clusters();
    cl->set_cluster(0);
    protozero::PackedVarInt cpus;
    for (uint32_t cpu = 0; cpu < 4; ++cpu) {
      cpus.Append(cpu);
    }
    cl->set_cpus(cpus);
    cl->set_deep_idle_offset_ns(0);
  }
  {
    auto* cl = model->add_cpu_clusters();
    cl->set_cluster(4);
    protozero::PackedVarInt cpus;
    for (uint32_t cpu = 4; cpu < 7; ++cpu) {
      cpus.Append(cpu);
    }
    cl->set_cpus(cpus);
  }
  {
    auto* cl = model->add_cpu_clusters();
    cl->set_cluster(7);
    protozero::PackedVarInt cpus;
    cpus.Append(7u);
    cl->set_cpus(cpus);
    cl->set_deep_idle_offset_ns(200000);
    cl->set_vote_by_freq(true);
  }

  // Idle state map: nominal 2 -> override 1
  {
    auto* idle = model->add_idle_state_map();
    idle->set_nominal_idle(2);
    idle->set_override_idle(1);
  }

  // CPU subsystem with 1D, DSU 1D, and 2D power curves
  {
    auto* cpu_sub = model->add_subsystems();
    cpu_sub->set_name("cpu");

    // 1D power curve: cluster 0 @ 1000000 kHz
    {
      auto* c1d = cpu_sub->add_curves();
      c1d->set_cluster(0);
      c1d->set_freq(1000000);
      c1d->set_static_mw(5.5f);
      protozero::PackedFixedSizeInt<float> active;
      active.Append(45.0f);
      c1d->set_active_mw(active);
      protozero::PackedFixedSizeInt<float> idle;
      idle.Append(1.25f);
      idle.Append(0.125f);
      c1d->set_idle_mw(idle);
    }

    // 1D power curve for a cluster whose curve set only characterises the
    // shallowest idle level: the deeper one has no entry in the array.
    {
      auto* c1d = cpu_sub->add_curves();
      c1d->set_cluster(4);
      c1d->set_freq(2000000);
      c1d->set_static_mw(9.5f);
      protozero::PackedFixedSizeInt<float> active;
      active.Append(120.0f);
      c1d->set_active_mw(active);
      protozero::PackedFixedSizeInt<float> idle;
      idle.Append(3.75f);
      c1d->set_idle_mw(idle);
    }

    // 1D DSU curve (cluster 255): active_mw only, no idle_mw.
    {
      auto* dsu = cpu_sub->add_curves();
      dsu->set_cluster(255);
      dsu->set_freq(394000000);
      protozero::PackedFixedSizeInt<float> active;
      active.Append(56.5f);
      dsu->set_active_mw(active);
    }

    // 2D power curve with merged L3 hit/miss: cluster 7 @ 3000000 kHz,
    // dep_cluster 4 @ 2000000 kHz
    {
      auto* c2d = cpu_sub->add_curves();
      c2d->set_cluster(7);
      c2d->set_freq(3000000);
      c2d->set_dep_cluster(4);
      c2d->set_dep_freq(2000000);
      c2d->set_static_mw(25.0f);
      protozero::PackedFixedSizeInt<float> active;
      active.Append(350.0f);
      c2d->set_active_mw(active);
      protozero::PackedFixedSizeInt<float> idle;
      idle.Append(2.5f);
      idle.Append(0.25f);
      c2d->set_idle_mw(idle);
      c2d->set_interconnect_mw(18.0f);
      c2d->set_l3_hit(12.5f);
      c2d->set_l3_miss(34.0f);
    }
  }

  // GPU subsystem
  {
    auto* gpu_sub = model->add_subsystems();
    gpu_sub->set_name("gpu");
    auto* gpu = gpu_sub->add_curves();
    gpu->set_freq(800000);
    protozero::PackedFixedSizeInt<float> active;
    active.Append(280.0f);
    gpu->set_active_mw(active);
    protozero::PackedFixedSizeInt<float> idle;
    idle.Append(6.0f);
    idle.Append(1.5f);
    gpu->set_idle_mw(idle);
  }

  // TPU subsystem (requests 0 and 1 packed into active_mw)
  {
    auto* tpu_sub = model->add_subsystems();
    tpu_sub->set_name("tpu");
    auto* tpu = tpu_sub->add_curves();
    tpu->set_cluster(0);
    tpu->set_freq(1200000);
    protozero::PackedFixedSizeInt<float> active;
    active.Append(210.0f);
    active.Append(420.0f);
    tpu->set_active_mw(active);
  }

  auto model_bytes = model.SerializeAsArray();

  // Wrap in TracePacket
  auto* packet = trace->add_packet();
  packet->set_timestamp(1000);
  packet->AppendBytes(FrameworksBaseTracePacket::kWattsonSocModelFieldNumber,
                      reinterpret_cast<const char*>(model_bytes.data()),
                      model_bytes.size());

  auto trace_bytes = trace.SerializeAsArray();
  Tokenize(trace_bytes.data(), trace_bytes.size());

  // 1. Verify Device Info Table
  const auto& dev_table = tables_->device_info;
  ASSERT_EQ(dev_table.row_count(), 1u);
  EXPECT_STREQ(storage_->GetString(dev_table[0].device_name()).c_str(),
               "Tensor G6");
  EXPECT_EQ(dev_table[0].use_devfreq(), 1);
  EXPECT_EQ(dev_table[0].gpu_id().value_or(-1), 0);
  EXPECT_EQ(dev_table[0].model_version().value_or(-1), 1);
  EXPECT_EQ(dev_table[0].model_source().value_or(-1),
            WattsonSocModel::MODEL_SOURCE_VENDOR);

  // 2. Verify CPU Clusters Table (including deep_idle_offset_ns & vote_by_freq)
  const auto& cluster_table = tables_->cpu_cluster;
  ASSERT_EQ(cluster_table.row_count(), 8u);
  EXPECT_EQ(cluster_table[0].cpu(), 0);
  EXPECT_EQ(cluster_table[0].cluster(), 0);
  EXPECT_EQ(cluster_table[0].deep_idle_offset_ns(), 0);
  EXPECT_EQ(cluster_table[0].vote_by_freq(), 0);
  EXPECT_EQ(cluster_table[4].cpu(), 4);
  EXPECT_EQ(cluster_table[4].cluster(), 4);
  EXPECT_EQ(cluster_table[4].deep_idle_offset_ns(), 0);
  EXPECT_EQ(cluster_table[4].vote_by_freq(), 0);
  EXPECT_EQ(cluster_table[7].cpu(), 7);
  EXPECT_EQ(cluster_table[7].cluster(), 7);
  EXPECT_EQ(cluster_table[7].deep_idle_offset_ns(), 200000);
  EXPECT_EQ(cluster_table[7].vote_by_freq(), 1);

  // 3. Verify Idle State Map
  const auto& idle_table = tables_->idle_state_map;
  ASSERT_EQ(idle_table.row_count(), 1u);
  EXPECT_EQ(idle_table[0].nominal_idle(), 2);
  EXPECT_EQ(idle_table[0].override_idle(), 1);

  // 4. Verify 1D Curves (including DSU cluster 255 without idle_mw)
  const auto& c1d_table = tables_->curves_cpu_1d;
  ASSERT_EQ(c1d_table.row_count(), 3u);
  EXPECT_EQ(c1d_table[0].cluster(), 0);
  EXPECT_EQ(c1d_table[0].freq_khz(), 1000000);
  EXPECT_THAT(c1d_table[0].static_mw(), DoubleEq(5.5));
  EXPECT_THAT(c1d_table[0].active(), DoubleEq(45.0));
  EXPECT_THAT(c1d_table[0].idle0(), DoubleEq(1.25));
  EXPECT_THAT(c1d_table[0].idle1(), DoubleEq(0.125));
  // The second curve only characterises the shallowest idle level, so the
  // deeper one stays at 0.
  EXPECT_EQ(c1d_table[1].cluster(), 4);
  EXPECT_THAT(c1d_table[1].idle0(), DoubleEq(3.75));
  EXPECT_THAT(c1d_table[1].idle1(), DoubleEq(0.0));
  // DSU 1D curve (cluster 255) has active_mw only; idle columns stay at 0.
  EXPECT_EQ(c1d_table[2].cluster(), 255);
  EXPECT_EQ(c1d_table[2].freq_khz(), 394000000);
  EXPECT_THAT(c1d_table[2].active(), DoubleEq(56.5));
  EXPECT_THAT(c1d_table[2].idle0(), DoubleEq(0.0));
  EXPECT_THAT(c1d_table[2].idle1(), DoubleEq(0.0));

  // 5. Verify 2D Curves
  const auto& c2d_table = tables_->curves_cpu_2d;
  ASSERT_EQ(c2d_table.row_count(), 1u);
  EXPECT_EQ(c2d_table[0].cluster(), 7);
  EXPECT_EQ(c2d_table[0].freq_khz(), 3000000);
  EXPECT_EQ(c2d_table[0].dep_cluster(), 4);
  EXPECT_EQ(c2d_table[0].dep_freq(), 2000000);
  EXPECT_THAT(c2d_table[0].static_mw(), DoubleEq(25.0));
  EXPECT_THAT(c2d_table[0].active(), DoubleEq(350.0));
  EXPECT_THAT(c2d_table[0].idle0(), DoubleEq(2.5));
  EXPECT_THAT(c2d_table[0].idle1(), DoubleEq(0.25));
  EXPECT_THAT(c2d_table[0].interconnect(), DoubleEq(18.0));

  // 6. Verify L3 Curves
  const auto& l3_table = tables_->curves_l3;
  ASSERT_EQ(l3_table.row_count(), 1u);
  EXPECT_EQ(l3_table[0].freq_khz(), 3000000);
  EXPECT_EQ(l3_table[0].dep_cluster(), 4);
  EXPECT_EQ(l3_table[0].dep_freq(), 2000000);
  EXPECT_THAT(l3_table[0].l3_hit(), DoubleEq(12.5));
  EXPECT_THAT(l3_table[0].l3_miss(), DoubleEq(34.0));

  // 7. Verify GPU Curves. The GPU curve set starts at idle level 1, so the
  // shallowest entry of the array lands in `idle1`.
  const auto& gpu_table = tables_->curves_gpu;
  ASSERT_EQ(gpu_table.row_count(), 1u);
  EXPECT_EQ(gpu_table[0].freq_khz(), 800000);
  EXPECT_THAT(gpu_table[0].active(), DoubleEq(280.0));
  EXPECT_THAT(gpu_table[0].idle1(), DoubleEq(6.0));
  EXPECT_THAT(gpu_table[0].idle2(), DoubleEq(1.5));

  // 8. Verify TPU Curves
  const auto& tpu_table = tables_->curves_tpu;
  ASSERT_EQ(tpu_table.row_count(), 2u);
  EXPECT_EQ(tpu_table[0].cluster(), 0);
  EXPECT_EQ(tpu_table[0].requests(), 0);
  EXPECT_EQ(tpu_table[0].freq(), 1200000);
  EXPECT_THAT(tpu_table[0].active(), DoubleEq(210.0));
  EXPECT_EQ(tpu_table[1].cluster(), 0);
  EXPECT_EQ(tpu_table[1].requests(), 1);
  EXPECT_EQ(tpu_table[1].freq(), 1200000);
  EXPECT_THAT(tpu_table[1].active(), DoubleEq(420.0));
}

TEST_F(WattsonSocModelModuleTest, IgnoresDuplicatePackets) {
  protozero::HeapBuffered<protos::pbzero::Trace> trace;

  // Packet 1: Tensor G6
  {
    protozero::HeapBuffered<WattsonSocModel> model;
    model->set_device_name("Tensor G6");
    auto* cpu_sub = model->add_subsystems();
    cpu_sub->set_name("cpu");
    auto* c1d = cpu_sub->add_curves();
    c1d->set_cluster(0);
    c1d->set_freq(1000000);
    protozero::PackedFixedSizeInt<float> active;
    active.Append(45.0f);
    c1d->set_active_mw(active);
    protozero::PackedFixedSizeInt<float> idle;
    idle.Append(1.25f);
    c1d->set_idle_mw(idle);
    auto model_bytes = model.SerializeAsArray();

    auto* packet = trace->add_packet();
    packet->set_timestamp(1000);
    packet->AppendBytes(FrameworksBaseTracePacket::kWattsonSocModelFieldNumber,
                        reinterpret_cast<const char*>(model_bytes.data()),
                        model_bytes.size());
  }

  // Packet 2: Duplicate / Second packet in the same trace should be ignored
  {
    protozero::HeapBuffered<WattsonSocModel> model;
    model->set_device_name("Tensor G5");
    auto* cpu_sub = model->add_subsystems();
    cpu_sub->set_name("cpu");
    auto* c1d = cpu_sub->add_curves();
    c1d->set_cluster(0);
    c1d->set_freq(2000000);
    protozero::PackedFixedSizeInt<float> active;
    active.Append(90.0f);
    c1d->set_active_mw(active);
    protozero::PackedFixedSizeInt<float> idle;
    idle.Append(2.5f);
    c1d->set_idle_mw(idle);
    auto model_bytes = model.SerializeAsArray();

    auto* packet = trace->add_packet();
    packet->set_timestamp(2000);
    packet->AppendBytes(FrameworksBaseTracePacket::kWattsonSocModelFieldNumber,
                        reinterpret_cast<const char*>(model_bytes.data()),
                        model_bytes.size());
  }

  auto trace_bytes = trace.SerializeAsArray();
  Tokenize(trace_bytes.data(), trace_bytes.size());

  const auto& dev_table = tables_->device_info;
  ASSERT_EQ(dev_table.row_count(), 1u);
  EXPECT_STREQ(storage_->GetString(dev_table[0].device_name()).c_str(),
               "Tensor G6");

  const auto& c1d_table = tables_->curves_cpu_1d;
  ASSERT_EQ(c1d_table.row_count(), 1u);
  EXPECT_EQ(c1d_table[0].freq_khz(), 1000000);
}

TEST_F(WattsonSocModelModuleTest, RejectsMalformedCurves) {
  protozero::HeapBuffered<protos::pbzero::Trace> trace;
  protozero::HeapBuffered<WattsonSocModel> model;
  model->set_device_name("Tensor G6");

  {
    auto* cpu_sub = model->add_subsystems();
    cpu_sub->set_name("cpu");

    // 1. Regular CPU 1D curve missing idle_mw -> skipped.
    {
      auto* c = cpu_sub->add_curves();
      c->set_cluster(0);
      c->set_freq(1000000);
      protozero::PackedFixedSizeInt<float> active;
      active.Append(45.0f);
      c->set_active_mw(active);
    }

    // 2. Regular CPU 1D curve missing active_mw -> skipped.
    {
      auto* c = cpu_sub->add_curves();
      c->set_cluster(0);
      c->set_freq(1100000);
      protozero::PackedFixedSizeInt<float> idle;
      idle.Append(1.25f);
      c->set_idle_mw(idle);
    }

    // 3. Unpaired dep_cluster without dep_freq -> skipped.
    {
      auto* c = cpu_sub->add_curves();
      c->set_cluster(7);
      c->set_freq(3000000);
      c->set_dep_cluster(4);
      protozero::PackedFixedSizeInt<float> active;
      active.Append(350.0f);
      c->set_active_mw(active);
      protozero::PackedFixedSizeInt<float> idle;
      idle.Append(2.5f);
      c->set_idle_mw(idle);
    }

    // 4. Unpaired dep_freq without dep_cluster -> skipped.
    {
      auto* c = cpu_sub->add_curves();
      c->set_cluster(7);
      c->set_freq(3100000);
      c->set_dep_freq(2000000);
      protozero::PackedFixedSizeInt<float> active;
      active.Append(350.0f);
      c->set_active_mw(active);
      protozero::PackedFixedSizeInt<float> idle;
      idle.Append(2.5f);
      c->set_idle_mw(idle);
    }

    // 5. Pure L3 2D curve without active_mw / idle_mw / interconnect_mw ->
    // populates L3 table only, not Cpu2D table.
    {
      auto* c = cpu_sub->add_curves();
      c->set_cluster(0);
      c->set_freq(820000);
      c->set_dep_cluster(255);
      c->set_dep_freq(610000);
      c->set_l3_hit(0.5f);
      c->set_l3_miss(0.875f);
    }

    // 6. Unpaired l3_hit without l3_miss -> skipped.
    {
      auto* c = cpu_sub->add_curves();
      c->set_cluster(0);
      c->set_freq(955000);
      c->set_dep_cluster(255);
      c->set_dep_freq(610000);
      c->set_l3_hit(0.5f);
    }
  }

  // 7. GPU curve missing idle_mw -> skipped.
  {
    auto* gpu_sub = model->add_subsystems();
    gpu_sub->set_name("gpu");
    auto* gpu = gpu_sub->add_curves();
    gpu->set_freq(800000);
    protozero::PackedFixedSizeInt<float> active;
    active.Append(280.0f);
    gpu->set_active_mw(active);
  }

  // 8. TPU curve missing cluster -> skipped.
  {
    auto* tpu_sub = model->add_subsystems();
    tpu_sub->set_name("tpu");
    auto* tpu = tpu_sub->add_curves();
    tpu->set_freq(1200000);
    protozero::PackedFixedSizeInt<float> active;
    active.Append(210.0f);
    tpu->set_active_mw(active);
  }

  auto model_bytes = model.SerializeAsArray();
  auto* packet = trace->add_packet();
  packet->set_timestamp(1000);
  packet->AppendBytes(FrameworksBaseTracePacket::kWattsonSocModelFieldNumber,
                      reinterpret_cast<const char*>(model_bytes.data()),
                      model_bytes.size());

  auto trace_bytes = trace.SerializeAsArray();
  Tokenize(trace_bytes.data(), trace_bytes.size());

  EXPECT_EQ(tables_->curves_cpu_1d.row_count(), 0u);
  EXPECT_EQ(tables_->curves_cpu_2d.row_count(), 0u);
  EXPECT_EQ(tables_->curves_gpu.row_count(), 0u);
  EXPECT_EQ(tables_->curves_tpu.row_count(), 0u);

  const auto& l3_table = tables_->curves_l3;
  ASSERT_EQ(l3_table.row_count(), 1u);
  EXPECT_EQ(l3_table[0].freq_khz(), 820000);
  EXPECT_EQ(l3_table[0].dep_cluster(), 255);
  EXPECT_EQ(l3_table[0].dep_freq(), 610000);
  EXPECT_THAT(l3_table[0].l3_hit(), DoubleEq(0.5));
  EXPECT_THAT(l3_table[0].l3_miss(), DoubleEq(0.875));
}

}  // namespace
}  // namespace perfetto::trace_processor::wattson
