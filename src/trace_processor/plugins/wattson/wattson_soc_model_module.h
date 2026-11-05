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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_WATTSON_WATTSON_SOC_MODEL_MODULE_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_WATTSON_WATTSON_SOC_MODEL_MODULE_H_

#include "perfetto/protozero/field.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/wattson/tables_py.h"

namespace perfetto::trace_processor {
class TraceProcessorContext;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::wattson {

// Holds the plugin-owned intrinsic tables populated from an out-of-tree
// WattsonSocModel TracePacket extension.
struct WattsonExternalTables {
  explicit WattsonExternalTables(StringPool* pool)
      : device_info(pool),
        cpu_cluster(pool),
        idle_state_map(pool),
        curves_cpu_1d(pool),
        curves_cpu_2d(pool),
        curves_l3(pool),
        curves_gpu(pool),
        curves_tpu(pool) {}

  tables::WattsonExternalDeviceInfoTable device_info;
  tables::WattsonExternalCpuClusterTable cpu_cluster;
  tables::WattsonExternalIdleStateMapTable idle_state_map;
  tables::WattsonExternalCurvesCpu1DTable curves_cpu_1d;
  tables::WattsonExternalCurvesCpu2DTable curves_cpu_2d;
  tables::WattsonExternalCurvesL3Table curves_l3;
  tables::WattsonExternalCurvesGpuTable curves_gpu;
  tables::WattsonExternalCurvesTpuTable curves_tpu;
};

class WattsonSocModelModule : public ProtoImporterModule {
 public:
  WattsonSocModelModule(ProtoImporterModuleContext* module_context,
                        TraceProcessorContext* context,
                        WattsonExternalTables* tables);
  ~WattsonSocModelModule() override;

  void ParseField(const ParseFieldArgs& args) override;

 private:
  void ParseWattsonSocModel(protozero::ConstBytes blob);
  void ParseCpuCluster(protozero::ConstBytes blob);
  void ParseIdleStateMap(protozero::ConstBytes blob);
  void ParseSubsystem(protozero::ConstBytes blob);
  void ParseCpuCurve(protozero::ConstBytes blob);
  void ParseGpuCurve(protozero::ConstBytes blob);
  void ParseTpuCurve(protozero::ConstBytes blob);

  TraceProcessorContext* const context_;
  WattsonExternalTables* const tables_;
};

}  // namespace perfetto::trace_processor::wattson

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_WATTSON_WATTSON_SOC_MODEL_MODULE_H_
