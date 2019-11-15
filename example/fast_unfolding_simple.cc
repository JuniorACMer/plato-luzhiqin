/*
  Tencent is pleased to support the open source community by making
  Plato available.
  Copyright (C) 2019 THL A29 Limited, a Tencent company.
  All rights reserved.

  Licensed under the BSD 3-Clause License (the "License"); you may
  not use this file except in compliance with the License. You may
  obtain a copy of the License at

  https://opensource.org/licenses/BSD-3-Clause

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" basis,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
  implied. See the License for the specific language governing
  permissions and limitations under the License.

  See the AUTHORS file for names of contributors.
*/

#include <cstdint>
#include <cstdlib>

#include "glog/logging.h"
#include "gflags/gflags.h"

#include "boost/format.hpp"
#include "boost/iostreams/stream.hpp"
#include "boost/iostreams/filter/gzip.hpp"
#include "boost/iostreams/filtering_stream.hpp"

#include "plato/graph/graph.hpp"
#include "plato/algo/fast_unfolding/fast_unfolding.hpp"

DEFINE_string(input,       "",     "input file, in csv format, without edge data");
DEFINE_string(output,       "",    "output directory, store the closeness result");
DEFINE_bool(is_directed,   false,  "is graph directed or not");
DEFINE_int32(alpha,        -1,     "alpha value used in sequence balance partition");
DEFINE_bool(part_by_in,    false,  "partition by in-degree");
DEFINE_int32(outer_iteration, 3,   "outer iteration of algorithm");
DEFINE_int32(inner_iteration, 2,   "inner iteration of algorithm");

void init(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::LogToStderr();
}

int main(int argc, char** argv) {
  plato::stop_watch_t watch;
  auto& cluster_info = plato::cluster_info_t::get_instance();

  init(argc, argv);
  cluster_info.initialize(&argc, &argv);
  LOG(INFO) << "partitions: " << cluster_info.partitions_ << " partition_id: " << cluster_info.partition_id_ << std::endl;

  watch.mark("t0");
  plato::graph_info_t graph_info(FLAGS_is_directed);

  using edge_value_t = float;
  plato::decoder_with_default_t<edge_value_t> decoder((edge_value_t)1);
  auto graph = plato::create_bcsr_seqs_from_path<edge_value_t>(&graph_info, 
      FLAGS_input, plato::edge_format_t::CSV, decoder,
      FLAGS_alpha, FLAGS_part_by_in);
  using BCSR = plato::bcsr_t<edge_value_t, plato::sequence_balanced_by_source_t>;
  plato::algo::louvain_opts_t opts;
  opts.outer_iteration_ = FLAGS_outer_iteration;
  opts.inner_iteration_ = FLAGS_inner_iteration;
  LOG(INFO) << "outer_iteraion: " << opts.outer_iteration_ << " inner_iteraion: " << opts.inner_iteration_;
  plato::algo::louvain_fast_unfolding_t<BCSR> louvain(graph, graph_info, opts);
  louvain.compute();
  
  std::vector<plato::hdfs_t::fstream *> fsms;
  using louvain_stream_t = boost::iostreams::filtering_stream<boost::iostreams::output>;
  std::vector<louvain_stream_t*> fouts;
  for (int tid = 0; tid < cluster_info.threads_; ++tid) {
    char fn[FILENAME_MAX];
    sprintf(fn, "%s/part-%05d.csv", FLAGS_output.c_str(),
        (cluster_info.partition_id_ * cluster_info.threads_ + tid));

    plato::hdfs_t &fs = plato::hdfs_t::get_hdfs(fn);
    fsms.emplace_back(new plato::hdfs_t::fstream(fs, fn, true));
    fouts.emplace_back(new louvain_stream_t());
    //fouts.back()->push(boost::iostreams::gzip_compressor());
    fouts.back()->push(*fsms.back());
    LOG(INFO) << fn << "\n";
  }

  louvain.save(fouts);

  for (int i = 0; i < cluster_info.threads_; ++i) {
    delete fouts[i];
    delete fsms[i];
  }

  LOG(INFO) << "total cost: " << watch.show("t0") / 1000.0;
  return 0;
}

