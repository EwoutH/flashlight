/*
 * Copyright (c) Facebook, Inc. and its affiliates.  * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <exception>
#include <iomanip>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "flashlight/app/objdet/common/Defines.h"
#include "flashlight/app/objdet/criterion/SetCriterion.h"
#include "flashlight/app/objdet/dataset/BoxUtils.h"
#include "flashlight/app/objdet/dataset/Coco.h"
#include "flashlight/app/objdet/models/Resnet50Backbone.h"
#include "flashlight/app/objdet/nn/Detr.h"
#include "flashlight/app/objdet/nn/Transformer.h"
#include "flashlight/ext/common/DistributedUtils.h"
#include "flashlight/app/common/Runtime.h"
#include "flashlight/ext/common/Serializer.h"
#include "flashlight/ext/image/af/Transforms.h"
#include "flashlight/fl/meter/meters.h"
#include "flashlight/fl/optim/optim.h"
#include "flashlight/lib/common/String.h"

using namespace fl;
using namespace fl::ext::image;
using namespace fl::app::objdet;

using fl::ext::Serializer;
using fl::app::getRunFile;
using fl::app::serializeGflags;
using fl::lib::fileExists;
using fl::lib::format;
using fl::lib::getCurrentDate;

#define FL_LOG_MASTER(lvl) LOG_IF(lvl, (fl::getWorldRank() == 0))

DEFINE_string(
    data_dir,
    "/private/home/padentomasello/data/coco_new/",
    "Directory of imagenet data");
DEFINE_double(train_lr, 0.0001f, "Learning rate");
DEFINE_uint64(metric_iters, 5, "Print metric every");

DEFINE_double(train_wd, 1e-4f, "Weight decay");
DEFINE_uint64(train_epochs, 300, "train_epochs");
DEFINE_uint64(eval_iters, 1, "Run evaluation every n epochs");
DEFINE_int64(
    distributed_world_rank,
    0,
    "rank of the process (Used if distributed_rndv_filepath is not empty)");
DEFINE_int64(
    distributed_world_size,
    1,
    "total number of the process (Used if distributed_rndv_filepath is not empty)");
DEFINE_string(
    distributed_rndv_filepath,
    "",
    "Shared file path used for setting up rendezvous."
    "If empty, uses MPI to initialize.");
DEFINE_bool(distributed_enable, true, "Enable distributed training");
DEFINE_uint64(data_batch_size, 2, "Total batch size across all gpus");

DEFINE_string(
    eval_dir,
    "/private/home/padentomasello/data/coco/output/",
    "Directory to dump images to run evaluation script on");
DEFINE_bool(
    model_pretrained,
    true,
    "Whether to load model_pretrained backbone");
DEFINE_string(
    model_pytorch_init,
    "",
    "Directory to dump images to run evaluation script on");
DEFINE_string(
    flagsfile,
    "",
    "Directory to dump images to run evaluation script on");
DEFINE_string(
    exp_rundir,
    "",
    "Directory to dump images to run evaluation script on");
DEFINE_string(
    eval_command,
    "",
    "Command to run  on dumped tensors");
DEFINE_bool(eval_only, false, "Weather to just run eval");

// Utility function that overrides flags file with command line arguments
void parseCmdLineFlagsWrapper(int argc, char** argv) {
  LOG(INFO) << "Parsing command line flags";
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  if (!FLAGS_flagsfile.empty()) {
    LOG(INFO) << "Reading flags from file " << FLAGS_flagsfile;
    gflags::ReadFromFlagsFile(FLAGS_flagsfile, argv[0], true);
  }
  gflags::ParseCommandLineFlags(&argc, &argv, false);
}

void evalLoop(
    std::shared_ptr<Detr> model,
    std::shared_ptr<CocoDataset> dataset) {
  model->eval();
  int idx = 0;
  std::stringstream mkdir_command;
  mkdir_command << "mkdir -p " << FLAGS_eval_dir << fl::getWorldRank();
  system(mkdir_command.str().c_str());
  for (auto& sample : *dataset) {
    std::vector<Variable> input = {fl::Variable(sample.images, false),
                                   fl::Variable(sample.masks, false)};
    auto output = model->forward(input);
    std::stringstream ss;
    ss << FLAGS_eval_dir << fl::getWorldRank() << "/detection" << idx
       << ".array";
    auto outputFile = ss.str();
    int lastLayerIdx = output[0].dims(3) - 1;
    auto scores = output[0].array()(
        af::span, af::span, af::span, af::seq(lastLayerIdx, lastLayerIdx));
    auto bboxes = output[1].array()(
        af::span, af::span, af::span, af::seq(lastLayerIdx, lastLayerIdx));
    af::saveArray(
        "imageSizes", sample.originalImageSizes, outputFile.c_str(), false);
    af::saveArray("imageIds", sample.imageIds, outputFile.c_str(), true);
    af::saveArray("scores", scores, outputFile.c_str(), true);
    af::saveArray("bboxes", bboxes, outputFile.c_str(), true);
    idx++;
  }
  if (FLAGS_distributed_enable) {
    barrier();
  }
  if (fl::getWorldRank() == 0) {
    std::stringstream ss;
    ss << FLAGS_eval_command << " --dir " << FLAGS_eval_dir;
    int numAttempts = 10;
    for (int i = 0; i < numAttempts; i++) {
      int rv = system(ss.str().c_str());
      if (rv == 0) {
        break;
      }
      std::cout << "Eval failed, retrying in 5 seconds" << std::endl;
      sleep(5);
    }
  }
  if (FLAGS_distributed_enable) {
    barrier();
  }
  std::stringstream ss2;
  ss2 << "rm -rf " << FLAGS_eval_dir << fl::getWorldRank() << "/detection*";
  std::cout << "Removing tmp eval files Command: " << ss2.str() << std::endl;
  // system(ss2.str().c_str());
  model->train();
};

int main(int argc, char** argv) {
  fl::init();
  af::info();

  ///////////////////////////
  // Setup train / continue modes
  ///////////////////////////
  int runIdx = 1; // current #runs in this path
  std::string runPath; // current experiment path
  std::string reloadPath; // path to model to reload
  std::string runStatus = argv[1];
  int64_t startEpoch = 0;
  std::string exec(argv[0]);
  std::vector<std::string> argvs;
  for (int i = 0; i < argc; i++) {
    argvs.emplace_back(argv[i]);
  }
  gflags::SetUsageMessage(
      "Usage: \n " + exec + " train [flags]\n or " + exec +
      " continue [directory] [flags]\n or " + exec +
      " fork [directory/model] [flags]");
  // Saving checkpointing
  if (argc <= 1) {
    LOG(FATAL) << gflags::ProgramUsage();
  }
  if (runStatus == kTrainMode) {
    parseCmdLineFlagsWrapper(argc, argv);
    runPath = FLAGS_exp_rundir;
  } else if (runStatus == kContinueMode) {
    runPath = argv[2];
    while (fileExists(getRunFile("model_last.bin", runIdx, runPath))) {
      ++runIdx;
    }
    reloadPath = getRunFile("model_last.bin", runIdx - 1, runPath);
    LOG(INFO) << "reload path is " << reloadPath;
    std::unordered_map<std::string, std::string> cfg;
    std::string version;
    Serializer::load(reloadPath, version, cfg);
    auto flags = cfg.find(kGflags);
    if (flags == cfg.end()) {
      LOG(FATAL) << "Invalid config loaded from " << reloadPath;
    }
    LOG(INFO) << "Reading flags from config file " << reloadPath;
    gflags::ReadFlagsFromString(flags->second, gflags::GetArgv0(), true);
    gflags::ParseCommandLineFlags(&argc, &argv, false);
    parseCmdLineFlagsWrapper(argc, argv);
    auto epoch = cfg.find(kEpoch);
    if (epoch == cfg.end()) {
      LOG(WARNING) << "Did not find epoch to start from, starting from 0.";
    } else {
      startEpoch = std::stoi(epoch->second);
    }
  } else {
    LOG(FATAL) << gflags::ProgramUsage();
  }

  if (runPath.empty()) {
    LOG(FATAL)
        << "'runpath' specified by --exp_rundir, --runname cannot be empty";
  }
  const std::string cmdLine = fl::lib::join(" ", argvs);
  std::unordered_map<std::string, std::string> config = {
      {kProgramName, exec},
      {kCommandLine, cmdLine},
      {kGflags, serializeGflags()},
      // extra goodies
      {kUserName, fl::lib::getEnvVar("USER")},
      {kHostName, fl::lib::getEnvVar("HOSTNAME")},
      {kTimestamp, getCurrentDate() + ", " + getCurrentDate()},
      {kRunIdx, std::to_string(runIdx)},
      {kRunPath, runPath}};

  /////////////////////////
  // Setup distributed training
  ////////////////////////
  std::shared_ptr<fl::Reducer> reducer = nullptr;
  if (FLAGS_distributed_enable) {
    fl::ext::initDistributed(
        FLAGS_distributed_world_rank,
        FLAGS_distributed_world_size,
        8,
        FLAGS_distributed_rndv_filepath);

    reducer = std::make_shared<fl::CoalescingReducer>(1.0, true, true);
  }
  const int worldRank = fl::getWorldRank();
  const int worldSize = fl::getWorldSize();

  ////////////////////////////
  // Create models
  ////////////////////////////
  const int32_t modelDim = 256;
  const int32_t numHeads = 8;
  const int32_t numEncoderLayers = 6;
  const int32_t numDecoderLayers = 6;
  const int32_t mlpDim = 2048;
  const int32_t hiddenDim = modelDim;
  const int32_t numClasses = 91;
  const int32_t numQueries = 100;
  const float pDropout = 0.1;
  const bool auxLoss = false;
  std::shared_ptr<Resnet50Backbone> backbone;
  if (FLAGS_model_pretrained) {
    std::string modelPath =
        "/checkpoint/padentomasello/models/resnet50/pretrained2";
    fl::load(modelPath, backbone);
  } else {
    backbone = std::make_shared<Resnet50Backbone>();
  }
  auto transformer = std::make_shared<Transformer>(
      modelDim, numHeads, numEncoderLayers, numDecoderLayers, mlpDim, pDropout);

  auto detr = std::make_shared<Detr>(
      transformer, backbone, hiddenDim, numClasses, numQueries, auxLoss);

  // Trained
  // untrained but initializaed
  if (!FLAGS_model_pytorch_init.empty()) {
    std::cout << "Loading from pytorch intiialization path"
              << FLAGS_model_pytorch_init << std::endl;
    // std::string modelPath =
    // "/checkpoint/padentomasello/models/detr/from_pytorch";  std::string
    // modelPath =
    // "/checkpoint/padentomasello/models/detr/model_pytorch_initializaition";
    fl::load(FLAGS_model_pytorch_init, detr);
  }
  detr->train();

  /////////////////////////
  // Build criterion
  /////////////////////////
  const float setCostClass = 1.f;
  const float setCostBBox = 5.f;
  const float setCostGiou = 2.f;
  const float bboxLossCoef = 5.f;
  const float giouLossCoef = 2.f;

  auto matcher = HungarianMatcher(setCostClass, setCostBBox, setCostGiou);

  const std::unordered_map<std::string, float> lossWeightsBase = {
      {"lossCe", 1.f}, {"lossGiou", giouLossCoef}, {"lossBbox", bboxLossCoef}};

  std::unordered_map<std::string, float> lossWeights;
  for (int i = 0; i < numDecoderLayers; i++) {
    for (auto l : lossWeightsBase) {
      std::string key = l.first + "_" + std::to_string(i);
      lossWeights[key] = l.second;
    }
  }
  auto criterion = SetCriterion(numClasses, matcher, lossWeights, 0.1);
  auto weightDict = criterion.getWeightDict();

  ////////////////////
  // Optimizers
  ////////////////////
  const float beta1 = 0.9;
  const float beta2 = 0.999;
  const float epsilon = 1e-8;
  auto opt = std::make_shared<AdamOptimizer>(
      detr->paramsWithoutBackbone(),
      FLAGS_train_lr,
      beta1,
      beta2,
      epsilon,
      FLAGS_train_wd);
  auto opt2 = std::make_shared<AdamOptimizer>(
      detr->backboneParams(),
      FLAGS_train_lr * 0.1,
      beta1,
      beta2,
      epsilon,
      FLAGS_train_wd);
  auto lrScheduler = [&opt, &opt2](int epoch) {
    // Adjust learning rate every 30 epoch after 30
    const float newLr = FLAGS_train_lr * pow(0.1, epoch / 100);
    LOG(INFO) << "Setting learning rate to: " << newLr;
    opt->setLr(newLr);
    opt2->setLr(newLr * 0.1);
  };

  /////////////////////////
  // Create Datasets
  /////////////////////////
  const int64_t data_batch_size_per_gpu = FLAGS_data_batch_size;
  const int64_t prefetch_threads = 10;
  std::string coco_dir = FLAGS_data_dir;
  auto train_ds = std::make_shared<CocoDataset>(
      coco_dir + "train.lst",
      worldRank,
      worldSize,
      data_batch_size_per_gpu,
      prefetch_threads,
      data_batch_size_per_gpu,
      false);

  auto val_ds = std::make_shared<CocoDataset>(
      coco_dir + "val.lst",
      worldRank,
      worldSize,
      data_batch_size_per_gpu,
      prefetch_threads,
      data_batch_size_per_gpu,
      true);

  // Override any initialization if continuing
  if (runStatus == "continue") {
    std::unordered_map<std::string, std::string> cfg; // unused
    std::string version;
    Serializer::load(reloadPath, version, cfg, detr, opt, opt2);
  }

  // Run eval if continuing
  if (startEpoch > 0 || FLAGS_eval_only) {
    detr->eval();
    evalLoop(detr, val_ds);
    detr->train();
    if (FLAGS_eval_only) {
      return 0;
    }
  }
  if (FLAGS_distributed_enable) {
    // synchronize parameters of the model so that the parameters in each
    // process is the same
    fl::allReduceParameters(detr);

    // Add a hook to synchronize gradients of model parameters as they are
    // computed
    fl::distributeModuleGrads(detr, reducer);
  }

  ////////////////
  // Training loop
  //////////////
  for (int epoch = startEpoch; epoch < FLAGS_train_epochs; epoch++) {
    int idx = 0;
    std::map<std::string, AverageValueMeter> meters;
    std::map<std::string, TimeMeter> timers;

    timers["total"].resume();
    lrScheduler(epoch);
    train_ds->resample();
    for (auto& sample : *train_ds) {
      std::vector<Variable> input = {fl::Variable(sample.images, false),
                                     fl::Variable(sample.masks, false)};
      auto output = detr->forward(input);

      timers["forward"].stop();

      /////////////////////////
      // Criterion
      /////////////////////////
      std::vector<Variable> targetBoxes(sample.target_boxes.size());
      std::vector<Variable> targetClasses(sample.target_labels.size());

      std::transform(
          sample.target_boxes.begin(),
          sample.target_boxes.end(),
          targetBoxes.begin(),
          [](const af::array& in) { return fl::Variable(in, false); });

      std::transform(
          sample.target_labels.begin(),
          sample.target_labels.end(),
          targetClasses.begin(),
          [](const af::array& in) { return fl::Variable(in, false); });

      timers["criterion"].resume();

      auto loss =
          criterion.forward(output[1], output[0], targetBoxes, targetClasses);

      auto accumLoss = fl::Variable(af::constant(0, 1), true);
      for (auto losses : loss) {
        fl::Variable scaled_loss = weightDict[losses.first] * losses.second;
        meters[losses.first].add(losses.second.array());
        meters[losses.first + "_weighted"].add(scaled_loss.array());
        accumLoss = scaled_loss + accumLoss;
      }
      meters["sum"].add(accumLoss.array());
      timers["criterion"].stop();

      /////////////////////////
      // Backward and update gradients
      //////////////////////////
      timers["backward"].resume();
      accumLoss.backward();
      timers["backward"].stop();

      if (FLAGS_distributed_enable) {
        reducer->finalize();
      }

      fl::clipGradNorm(detr->params(), 0.1);

      opt->step();
      opt2->step();

      opt->zeroGrad();
      opt2->zeroGrad();
      //////////////////////////
      // Metrics
      /////////////////////////
      if (++idx % FLAGS_metric_iters == 0) {
        double total_time = timers["total"].value();
        double sample_per_second =
            (idx * FLAGS_data_batch_size * worldSize) / total_time;
        double forward_time = timers["forward"].value();
        double backward_time = timers["backward"].value();
        double criterion_time = timers["criterion"].value();
        std::stringstream ss;
        ss << "Epoch: " << epoch << std::setprecision(5) << " | Batch: " << idx
           << " | total_time: " << total_time << " | idx: " << idx
           << " | sample_per_second: " << sample_per_second
           << " | forward_time_avg: " << forward_time / idx
           << " | backward_time_avg: " << backward_time / idx
           << " | criterion_time_avg: " << criterion_time / idx;
        for (auto meter : meters) {
          fl::ext::syncMeter(meter.second);
          ss << " | " << meter.first << ": " << meter.second.value()[0];
        }
        ss << std::endl;
        FL_LOG_MASTER(INFO) << ss.str();
      }
    }
    for (auto timer : timers) {
      timer.second.reset();
    }
    for (auto meter : meters) {
      meter.second.reset();
    }
    if (fl::getWorldRank() == 0) {
      std::string filename =
          getRunFile(format("model_last.bin", idx), runIdx, runPath);
      config[kEpoch] = std::to_string(epoch);
      Serializer::save(filename, "0.1", config, detr, opt, opt2);
      filename =
          getRunFile(format("model_iter_%03d.bin", epoch), runIdx, runPath);
      Serializer::save(filename, "0.1", config, detr, opt, opt2);
    }
    if (epoch % FLAGS_eval_iters == 0) {
      evalLoop(detr, val_ds);
    }
  }
}
