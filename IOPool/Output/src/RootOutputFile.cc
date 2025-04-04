
#include "IOPool/Output/src/RootOutputFile.h"

#include "FWCore/Utilities/interface/GlobalIdentifier.h"

#include "DataFormats/Provenance/interface/EventAuxiliary.h"
#include "DataFormats/Provenance/interface/ProductDescription.h"
#include "FWCore/Version/interface/GetFileFormatVersion.h"
#include "DataFormats/Provenance/interface/FileFormatVersion.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "FWCore/Utilities/interface/Algorithms.h"
#include "FWCore/Utilities/interface/Digest.h"
#include "FWCore/Common/interface/OutputProcessBlockHelper.h"
#include "FWCore/Framework/interface/FileBlock.h"
#include "FWCore/Framework/interface/EventForOutput.h"
#include "FWCore/Framework/interface/LuminosityBlockForOutput.h"
#include "FWCore/Framework/interface/MergeableRunProductMetadata.h"
#include "FWCore/Framework/interface/OccurrenceForOutput.h"
#include "FWCore/Framework/interface/ProcessBlockForOutput.h"
#include "FWCore/Framework/interface/RunForOutput.h"
#include "FWCore/MessageLogger/interface/JobReport.h"
#include "FWCore/MessageLogger/interface/MessageLogger.h"
#include "DataFormats/Common/interface/BasicHandle.h"
#include "DataFormats/Provenance/interface/ProductDependencies.h"
#include "DataFormats/Provenance/interface/BranchIDList.h"
#include "DataFormats/Provenance/interface/Parentage.h"
#include "DataFormats/Provenance/interface/ParentageRegistry.h"
#include "DataFormats/Provenance/interface/EventID.h"
#include "DataFormats/Provenance/interface/EventToProcessBlockIndexes.h"
#include "DataFormats/Provenance/interface/ParameterSetBlob.h"
#include "DataFormats/Provenance/interface/ParameterSetID.h"
#include "DataFormats/Provenance/interface/ProcessHistoryID.h"
#include "DataFormats/Provenance/interface/ProductRegistry.h"
#include "DataFormats/Provenance/interface/StoredProcessBlockHelper.h"
#include "DataFormats/Provenance/interface/ThinnedAssociationsHelper.h"
#include "DataFormats/Provenance/interface/ProductRegistry.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/Registry.h"
#include "FWCore/ServiceRegistry/interface/Service.h"
#include "FWCore/Utilities/interface/ExceptionPropagate.h"
#include "IOPool/Common/interface/getWrapperBasePtr.h"
#include "IOPool/Provenance/interface/CommonProvenanceFiller.h"

#include "TTree.h"
#include "TFile.h"
#include "TClass.h"
#include "Rtypes.h"
#include "RVersion.h"

#include "Compression.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace edm {

  namespace {
    bool sorterForJobReportHash(ProductDescription const* lh, ProductDescription const* rh) {
      return lh->fullClassName() < rh->fullClassName()               ? true
             : lh->fullClassName() > rh->fullClassName()             ? false
             : lh->moduleLabel() < rh->moduleLabel()                 ? true
             : lh->moduleLabel() > rh->moduleLabel()                 ? false
             : lh->productInstanceName() < rh->productInstanceName() ? true
             : lh->productInstanceName() > rh->productInstanceName() ? false
             : lh->processName() < rh->processName()                 ? true
                                                                     : false;
    }

    TFile* openTFile(char const* name, int compressionLevel) {
      TFile* file = TFile::Open(name, "recreate", "", compressionLevel);
      std::exception_ptr e = edm::threadLocalException::getException();
      if (e != std::exception_ptr()) {
        edm::threadLocalException::setException(std::exception_ptr());
        std::rethrow_exception(e);
      }
      return file;
    }
  }  // namespace

  RootOutputFile::RootOutputFile(PoolOutputModule* om,
                                 std::string const& fileName,
                                 std::string const& logicalFileName,
                                 std::vector<std::string> const& processesWithSelectedMergeableRunProducts,
                                 std::string const& overrideGUID)
      : file_(fileName),
        logicalFile_(logicalFileName),
        reportToken_(0),
        om_(om),
        whyNotFastClonable_(om_->whyNotFastClonable()),
        canFastCloneAux_(false),
        filePtr_(openTFile(file_.c_str(), om_->compressionLevel())),
        fid_(),
        eventEntryNumber_(0LL),
        lumiEntryNumber_(0LL),
        runEntryNumber_(0LL),
        indexIntoFile_(),
        storedMergeableRunProductMetadata_(processesWithSelectedMergeableRunProducts),
        nEventsInLumi_(0),
        metaDataTree_(nullptr),
        parameterSetsTree_(nullptr),
        parentageTree_(nullptr),
        lumiAux_(),
        runAux_(),
        pEventAux_(nullptr),
        pLumiAux_(&lumiAux_),
        pRunAux_(&runAux_),
        eventEntryInfoVector_(),
        pEventEntryInfoVector_(&eventEntryInfoVector_),
        pBranchListIndexes_(nullptr),
        pEventSelectionIDs_(nullptr),
        eventTree_(filePtr(), InEvent, om_->splitLevel(), om_->treeMaxVirtualSize()),
        lumiTree_(filePtr(), InLumi, om_->splitLevel(), om_->treeMaxVirtualSize()),
        runTree_(filePtr(), InRun, om_->splitLevel(), om_->treeMaxVirtualSize()),
        dataTypeReported_(false),
        processHistoryRegistry_(),
        parentageIDs_(),
        branchesWithStoredHistory_(),
        wrapperBaseTClass_(TClass::GetClass("edm::WrapperBase")) {
    std::vector<std::string> const& processesWithProcessBlockProducts =
        om_->outputProcessBlockHelper().processesWithProcessBlockProducts();
    for (auto const& processName : processesWithProcessBlockProducts) {
      processBlockTrees_.emplace_back(std::make_unique<RootOutputTree>(
          filePtr(), InProcess, om_->splitLevel(), om_->treeMaxVirtualSize(), processName));
    }

    if (om_->compressionAlgorithm() == std::string("ZLIB")) {
      filePtr_->SetCompressionAlgorithm(ROOT::RCompressionSetting::EAlgorithm::kZLIB);
    } else if (om_->compressionAlgorithm() == std::string("LZMA")) {
      filePtr_->SetCompressionAlgorithm(ROOT::RCompressionSetting::EAlgorithm::kLZMA);
    } else if (om_->compressionAlgorithm() == std::string("ZSTD")) {
      filePtr_->SetCompressionAlgorithm(ROOT::RCompressionSetting::EAlgorithm::kZSTD);
    } else if (om_->compressionAlgorithm() == std::string("LZ4")) {
      filePtr_->SetCompressionAlgorithm(ROOT::RCompressionSetting::EAlgorithm::kLZ4);
    } else {
      throw Exception(errors::Configuration)
          << "PoolOutputModule configured with unknown compression algorithm '" << om_->compressionAlgorithm() << "'\n"
          << "Allowed compression algorithms are ZLIB, LZMA, LZ4, and ZSTD\n";
    }
    if (-1 != om->eventAutoFlushSize()) {
      eventTree_.setAutoFlush(-1 * om->eventAutoFlushSize());
    }
    if (om_->compactEventAuxiliary()) {
      eventTree_.addAuxiliary<EventAuxiliary>(
          BranchTypeToAuxiliaryBranchName(InEvent), pEventAux_, om_->auxItems()[InEvent].basketSize_, false);
      eventTree_.tree()->SetBranchStatus(BranchTypeToAuxiliaryBranchName(InEvent).c_str(),
                                         false);  // see writeEventAuxiliary
    } else {
      eventTree_.addAuxiliary<EventAuxiliary>(
          BranchTypeToAuxiliaryBranchName(InEvent), pEventAux_, om_->auxItems()[InEvent].basketSize_);
    }

    eventTree_.addAuxiliary<StoredProductProvenanceVector>(BranchTypeToProductProvenanceBranchName(InEvent),
                                                           pEventEntryInfoVector(),
                                                           om_->auxItems()[InEvent].basketSize_);
    eventTree_.addAuxiliary<EventSelectionIDVector>(
        poolNames::eventSelectionsBranchName(), pEventSelectionIDs_, om_->auxItems()[InEvent].basketSize_, false);
    eventTree_.addAuxiliary<BranchListIndexes>(
        poolNames::branchListIndexesBranchName(), pBranchListIndexes_, om_->auxItems()[InEvent].basketSize_);

    if (om_->outputProcessBlockHelper().productsFromInputKept()) {
      eventTree_.addAuxiliary<EventToProcessBlockIndexes>(poolNames::eventToProcessBlockIndexesBranchName(),
                                                          pEventToProcessBlockIndexes_,
                                                          om_->auxItems()[InEvent].basketSize_);
    }

    lumiTree_.addAuxiliary<LuminosityBlockAuxiliary>(
        BranchTypeToAuxiliaryBranchName(InLumi), pLumiAux_, om_->auxItems()[InLumi].basketSize_);

    runTree_.addAuxiliary<RunAuxiliary>(
        BranchTypeToAuxiliaryBranchName(InRun), pRunAux_, om_->auxItems()[InRun].basketSize_);

    treePointers_.emplace_back(&eventTree_);
    treePointers_.emplace_back(&lumiTree_);
    treePointers_.emplace_back(&runTree_);
    for (auto& processBlockTree : processBlockTrees_) {
      treePointers_.emplace_back(processBlockTree.get());
    }

    for (unsigned int i = 0; i < treePointers_.size(); ++i) {
      RootOutputTree* theTree = treePointers_[i];
      for (auto& item : om_->selectedOutputItemList()[i]) {
        item.setProduct(nullptr);
        ProductDescription const& desc = *item.productDescription();
        theTree->addBranch(desc.branchName(),
                           desc.wrappedName(),
                           item.product(),
                           item.splitLevel(),
                           item.basketSize(),
                           item.productDescription()->produced());
        //make sure we always store product registry info for all branches we create
        branchesWithStoredHistory_.insert(item.branchID());
      }
    }
    // Don't split metadata tree or event description tree
    metaDataTree_ = RootOutputTree::makeTTree(filePtr_.get(), poolNames::metaDataTreeName(), 0);
    parentageTree_ = RootOutputTree::makeTTree(filePtr_.get(), poolNames::parentageTreeName(), 0);
    parameterSetsTree_ = RootOutputTree::makeTTree(filePtr_.get(), poolNames::parameterSetsTreeName(), 0);

    if (overrideGUID.empty()) {
      fid_ = FileID(createGlobalIdentifier());
    } else {
      if (not isValidGlobalIdentifier(overrideGUID)) {
        throw edm::Exception(errors::Configuration)
            << "GUID to be used for output file is not valid (is '" << overrideGUID << "')";
      }
      fid_ = FileID(overrideGUID);
    }

    // For the Job Report, get a vector of branch names in the "Events" tree.
    // Also create a hash of all the branch names in the "Events" tree
    // in a deterministic order, except use the full class name instead of the friendly class name.
    // To avoid extra string copies, we create a vector of pointers into the product registry,
    // and use a custom comparison operator for sorting.
    std::vector<std::string> branchNames;
    std::vector<ProductDescription const*> branches;
    branchNames.reserve(om_->selectedOutputItemList()[InEvent].size());
    branches.reserve(om->selectedOutputItemList()[InEvent].size());
    for (auto const& item : om_->selectedOutputItemList()[InEvent]) {
      branchNames.push_back(item.productDescription()->branchName());
      branches.push_back(item.productDescription());
    }
    // Now sort the branches for the hash.
    sort_all(branches, sorterForJobReportHash);
    // Now, make a concatenated string.
    std::ostringstream oss;
    char const underscore = '_';
    for (auto const& branch : branches) {
      ProductDescription const& bd = *branch;
      oss << bd.fullClassName() << underscore << bd.moduleLabel() << underscore << bd.productInstanceName()
          << underscore << bd.processName() << underscore;
    }
    std::string stringrep = oss.str();
    cms::Digest md5alg(stringrep);

    // Register the output file with the JobReport service
    // and get back the token for it.
    std::string moduleName = "PoolOutputModule";
    Service<JobReport> reportSvc;
    reportToken_ = reportSvc->outputFileOpened(file_,
                                               logicalFile_,        // PFN and LFN
                                               om_->catalog(),      // catalog
                                               moduleName,          // module class name
                                               om_->moduleLabel(),  // module label
                                               fid_.fid(),          // file id (guid)
                                               std::string(),       // data type (not yet known, so string is empty).
                                               md5alg.digest().toString(),  // branch hash
                                               branchNames);                // branch names being written
  }

  namespace {
    void maybeIssueWarning(int whyNotFastClonable, std::string const& ifileName, std::string const& ofileName) {
      // No message if fast cloning was deliberately disabled, or if there are no events to copy anyway.
      if ((whyNotFastClonable & (FileBlock::DisabledInConfigFile | FileBlock::NoRootInputSource |
                                 FileBlock::NotProcessingEvents | FileBlock::NoEventsInFile)) != 0) {
        return;
      }

      // There will be a message stating every reason that fast cloning was not possible.
      // If at one or more of the reasons was because of something the user explicitly specified (e.g. event selection, skipping events),
      // or if the input file was in an old format, the message will be informational.  Otherwise, the message will be a warning.
      bool isWarning = true;
      std::ostringstream message;
      message << "Fast copying of file " << ifileName << " to file " << ofileName << " is disabled because:\n";
      if ((whyNotFastClonable & FileBlock::HasSecondaryFileSequence) != 0) {
        message << "a SecondaryFileSequence was specified.\n";
        whyNotFastClonable &= ~(FileBlock::HasSecondaryFileSequence);
        isWarning = false;
      }
      if ((whyNotFastClonable & FileBlock::FileTooOld) != 0) {
        message << "the input file is in an old format.\n";
        whyNotFastClonable &= ~(FileBlock::FileTooOld);
        isWarning = false;
      }
      if ((whyNotFastClonable & FileBlock::EventsToBeSorted) != 0) {
        message << "events need to be sorted.\n";
        whyNotFastClonable &= ~(FileBlock::EventsToBeSorted);
      }
      if ((whyNotFastClonable & FileBlock::RunOrLumiNotContiguous) != 0) {
        message << "a run or a lumi is not contiguous in the input file.\n";
        whyNotFastClonable &= ~(FileBlock::RunOrLumiNotContiguous);
      }
      if ((whyNotFastClonable & FileBlock::EventsOrLumisSelectedByID) != 0) {
        message << "events or lumis were selected or skipped by ID.\n";
        whyNotFastClonable &= ~(FileBlock::EventsOrLumisSelectedByID);
        isWarning = false;
      }
      if ((whyNotFastClonable & FileBlock::InitialEventsSkipped) != 0) {
        message << "initial events, lumis or runs were skipped.\n";
        whyNotFastClonable &= ~(FileBlock::InitialEventsSkipped);
        isWarning = false;
      }
      if ((whyNotFastClonable & FileBlock::DuplicateEventsRemoved) != 0) {
        message << "some events were skipped because of duplicate checking.\n";
        whyNotFastClonable &= ~(FileBlock::DuplicateEventsRemoved);
      }
      if ((whyNotFastClonable & FileBlock::MaxEventsTooSmall) != 0) {
        message << "some events were not copied because of maxEvents limit.\n";
        whyNotFastClonable &= ~(FileBlock::MaxEventsTooSmall);
        isWarning = false;
      }
      if ((whyNotFastClonable & FileBlock::MaxLumisTooSmall) != 0) {
        message << "some events were not copied because of maxLumis limit.\n";
        whyNotFastClonable &= ~(FileBlock::MaxLumisTooSmall);
        isWarning = false;
      }
      if ((whyNotFastClonable & FileBlock::ParallelProcesses) != 0) {
        message << "parallel processing was specified.\n";
        whyNotFastClonable &= ~(FileBlock::ParallelProcesses);
        isWarning = false;
      }
      if ((whyNotFastClonable & FileBlock::EventSelectionUsed) != 0) {
        message << "an EventSelector was specified.\n";
        whyNotFastClonable &= ~(FileBlock::EventSelectionUsed);
        isWarning = false;
      }
      if ((whyNotFastClonable & FileBlock::OutputMaxEventsTooSmall) != 0) {
        message << "some events were not copied because of maxEvents output limit.\n";
        whyNotFastClonable &= ~(FileBlock::OutputMaxEventsTooSmall);
        isWarning = false;
      }
      if ((whyNotFastClonable & FileBlock::SplitLevelMismatch) != 0) {
        message << "the split level or basket size of a branch or branches was modified.\n";
        whyNotFastClonable &= ~(FileBlock::SplitLevelMismatch);
      }
      if ((whyNotFastClonable & FileBlock::BranchMismatch) != 0) {
        message << "The format of a data product has changed.\n";
        whyNotFastClonable &= ~(FileBlock::BranchMismatch);
      }
      assert(whyNotFastClonable == FileBlock::CanFastClone);
      if (isWarning) {
        LogWarning("FastCloningDisabled") << message.str();
      } else {
        LogInfo("FastCloningDisabled") << message.str();
      }
    }
  }  // namespace

  void RootOutputFile::beginInputFile(FileBlock const& fb, int remainingEvents) {
    // Reset per input file information
    whyNotFastClonable_ = om_->whyNotFastClonable();
    canFastCloneAux_ = false;

    if (fb.tree() != nullptr) {
      whyNotFastClonable_ |= fb.whyNotFastClonable();

      if (remainingEvents >= 0 && remainingEvents < fb.tree()->GetEntries()) {
        whyNotFastClonable_ |= FileBlock::OutputMaxEventsTooSmall;
      }

      bool match = eventTree_.checkSplitLevelsAndBasketSizes(fb.tree());
      if (!match) {
        if (om_->overrideInputFileSplitLevels()) {
          // We may be fast copying.  We must disable fast copying if the split levels
          // or basket sizes do not match.
          whyNotFastClonable_ |= FileBlock::SplitLevelMismatch;
        } else {
          // We are using the input split levels and basket sizes from the first input file
          // for copied output branches.  In this case, we throw an exception if any branches
          // have different split levels or basket sizes in a subsequent input file.
          // If the mismatch is in the first file, there is a bug somewhere, so we assert.
          assert(om_->inputFileCount() > 1);
          throw Exception(errors::MismatchedInputFiles, "RootOutputFile::beginInputFile()")
              << "Merge failure because input file " << file_ << " has different ROOT split levels or basket sizes\n"
              << "than previous files.  To allow merging in spite of this, use the configuration parameter\n"
              << "overrideInputFileSplitLevels=cms.untracked.bool(True)\n"
              << "in every PoolOutputModule.\n";
        }
      }

      // Since this check can be time consuming, we do it only if we would otherwise fast clone.
      if (whyNotFastClonable_ == FileBlock::CanFastClone) {
        if (!eventTree_.checkIfFastClonable(fb.tree())) {
          whyNotFastClonable_ |= FileBlock::BranchMismatch;
        }
      }

      // reasons for whyNotFastClonable that are also inconsistent with a merge job
      constexpr auto setSubBranchBasketConditions =
          FileBlock::EventsOrLumisSelectedByID | FileBlock::InitialEventsSkipped | FileBlock::MaxEventsTooSmall |
          FileBlock::MaxLumisTooSmall | FileBlock::EventSelectionUsed | FileBlock::OutputMaxEventsTooSmall |
          FileBlock::SplitLevelMismatch | FileBlock::BranchMismatch;

      if (om_->inputFileCount() == 1) {
        if (om_->mergeJob()) {
          // for merge jobs always forward the compression mode
          auto infile = fb.tree()->GetCurrentFile();
          if (infile != nullptr) {
            filePtr_->SetCompressionSettings(infile->GetCompressionSettings());
          }
        }

        // if we aren't fast cloning, and the reason why is consistent with a
        // merge job or is only because of parallel processes, then forward all
        // the sub-branch basket sizes
        if (whyNotFastClonable_ != FileBlock::CanFastClone &&
            ((om_->mergeJob() && (whyNotFastClonable_ & setSubBranchBasketConditions) == 0) ||
             (whyNotFastClonable_ == FileBlock::ParallelProcesses))) {
          eventTree_.setSubBranchBasketSizes(fb.tree());
        }
      }

      // We now check if we can fast copy the auxiliary branches.
      // We can do so only if we can otherwise fast copy,
      // the input file has the current format (these branches are in the Events Tree),
      // there are no newly dropped or produced products,
      // no metadata has been dropped,
      // ID's have not been modified,
      // and the branch list indexes do not need modification.

      // Note: Fast copy of the EventProductProvenance branch is unsafe
      // unless we can enforce that the parentage information for a fully copied
      // output file will be the same as for the input file, with nothing dropped.
      // This has never been enforced, and, withthe EDAlias feature, it may no longer
      // work by accident.
      // So, for now, we do not enable fast cloning of the non-product branches.
      /*
      canFastCloneAux_ = (whyNotFastClonable_ == FileBlock::CanFastClone) &&
                          fb.fileFormatVersion().noMetaDataTrees() &&
                          !om_->hasNewlyDroppedBranch()[InEvent] &&
                          !fb.hasNewlyDroppedBranch()[InEvent] &&
                          om_->dropMetaData() == PoolOutputModule::DropNone &&
                          !reg->anyProductProduced() &&
                          !fb.modifiedIDs() &&
                          fb.branchListIndexesUnchanged();
      */

      // Report the fast copying status.
      Service<JobReport> reportSvc;
      reportSvc->reportFastCopyingStatus(reportToken_, fb.fileName(), whyNotFastClonable_ == FileBlock::CanFastClone);
    } else {
      whyNotFastClonable_ |= FileBlock::NoRootInputSource;
    }

    eventTree_.maybeFastCloneTree(
        whyNotFastClonable_ == FileBlock::CanFastClone, canFastCloneAux_, fb.tree(), om_->basketOrder());

    // Possibly issue warning or informational message if we haven't fast cloned.
    if (fb.tree() != nullptr && whyNotFastClonable_ != FileBlock::CanFastClone) {
      maybeIssueWarning(whyNotFastClonable_, fb.fileName(), file_);
    }

    if (om_->compactEventAuxiliary() &&
        (whyNotFastClonable_ & (FileBlock::EventsOrLumisSelectedByID | FileBlock::InitialEventsSkipped |
                                FileBlock::EventSelectionUsed)) == 0) {
      long long int reserve = remainingEvents;
      if (fb.tree() != nullptr) {
        reserve = reserve > 0 ? std::min(fb.tree()->GetEntries(), reserve) : fb.tree()->GetEntries();
      }
      if (reserve > 0) {
        compactEventAuxiliary_.reserve(compactEventAuxiliary_.size() + reserve);
      }
    }
  }

  void RootOutputFile::respondToCloseInputFile(FileBlock const&) {
    // We can't do setEntries() on the event tree if the EventAuxiliary branch is empty & disabled
    if (not om_->compactEventAuxiliary()) {
      eventTree_.setEntries();
    }
    lumiTree_.setEntries();
    runTree_.setEntries();
  }

  bool RootOutputFile::shouldWeCloseFile() const {
    unsigned int const oneK = 1024;
    Long64_t size = filePtr_->GetSize() / oneK;
    return (size >= om_->maxFileSize());
  }

  void RootOutputFile::writeOne(EventForOutput const& e) {
    // Auxiliary branch
    pEventAux_ = &e.eventAuxiliary();

    // Because getting the data may cause an exception to be thrown we want to do that
    // first before writing anything to the file about this event
    // NOTE: pEventAux_, pBranchListIndexes_, pEventSelectionIDs_, and pEventEntryInfoVector_
    // must be set before calling fillBranches since they get written out in that routine.
    assert(pEventAux_->processHistoryID() == e.processHistoryID());
    pBranchListIndexes_ = &e.branchListIndexes();
    pEventToProcessBlockIndexes_ = &e.eventToProcessBlockIndexes();

    // Note: The EventSelectionIDVector should have a one to one correspondence with the processes in the process history.
    // Therefore, a new entry should be added if and only if the current process has been added to the process history,
    // which is done if and only if there is a produced product.
    EventSelectionIDVector esids = e.eventSelectionIDs();
    if (e.productRegistry().anyProductProduced() || !om_->wantAllEvents()) {
      esids.push_back(om_->selectorConfig());
    }
    pEventSelectionIDs_ = &esids;
    ProductProvenanceRetriever const* provRetriever = e.productProvenanceRetrieverPtr();
    assert(provRetriever);
    unsigned int ttreeIndex = InEvent;
    fillBranches(InEvent, e, ttreeIndex, pEventEntryInfoVector_, provRetriever);

    // Add the dataType to the job report if it hasn't already been done
    if (!dataTypeReported_) {
      Service<JobReport> reportSvc;
      std::string dataType("MC");
      if (pEventAux_->isRealData())
        dataType = "Data";
      reportSvc->reportDataType(reportToken_, dataType);
      dataTypeReported_ = true;
    }

    // Store the process history.
    processHistoryRegistry_.registerProcessHistory(e.processHistory());
    // Store the reduced ID in the IndexIntoFile
    ProcessHistoryID reducedPHID = processHistoryRegistry_.reducedProcessHistoryID(e.processHistoryID());
    // Add event to index
    indexIntoFile_.addEntry(
        reducedPHID, pEventAux_->run(), pEventAux_->luminosityBlock(), pEventAux_->event(), eventEntryNumber_);
    ++eventEntryNumber_;

    if (om_->compactEventAuxiliary()) {
      compactEventAuxiliary_.push_back(*pEventAux_);
    }

    // Report event written
    Service<JobReport> reportSvc;
    reportSvc->eventWrittenToFile(reportToken_, e.id().run(), e.id().event());
    ++nEventsInLumi_;
  }

  void RootOutputFile::writeLuminosityBlock(LuminosityBlockForOutput const& lb) {
    // Auxiliary branch
    // NOTE: lumiAux_ must be filled before calling fillBranches since it gets written out in that routine.
    lumiAux_ = lb.luminosityBlockAuxiliary();
    // Use the updated process historyID
    lumiAux_.setProcessHistoryID(lb.processHistoryID());
    // Store the process history.
    processHistoryRegistry_.registerProcessHistory(lb.processHistory());
    // Store the reduced ID in the IndexIntoFile
    ProcessHistoryID reducedPHID = processHistoryRegistry_.reducedProcessHistoryID(lb.processHistoryID());
    // Add lumi to index.
    indexIntoFile_.addEntry(reducedPHID, lumiAux_.run(), lumiAux_.luminosityBlock(), 0U, lumiEntryNumber_);
    ++lumiEntryNumber_;
    unsigned int ttreeIndex = InLumi;
    fillBranches(InLumi, lb, ttreeIndex);
    lumiTree_.optimizeBaskets(10ULL * 1024 * 1024);

    Service<JobReport> reportSvc;
    reportSvc->reportLumiSection(reportToken_, lb.id().run(), lb.id().luminosityBlock(), nEventsInLumi_);
    nEventsInLumi_ = 0;
  }

  void RootOutputFile::writeRun(RunForOutput const& r) {
    // Auxiliary branch
    // NOTE: runAux_ must be filled before calling fillBranches since it gets written out in that routine.
    runAux_ = r.runAuxiliary();
    // Use the updated process historyID
    runAux_.setProcessHistoryID(r.processHistoryID());
    // Store the process history.
    processHistoryRegistry_.registerProcessHistory(r.processHistory());
    // Store the reduced ID in the IndexIntoFile
    ProcessHistoryID reducedPHID = processHistoryRegistry_.reducedProcessHistoryID(r.processHistoryID());
    // Add run to index.
    indexIntoFile_.addEntry(reducedPHID, runAux_.run(), 0U, 0U, runEntryNumber_);
    r.mergeableRunProductMetadata()->addEntryToStoredMetadata(storedMergeableRunProductMetadata_);
    ++runEntryNumber_;
    unsigned int ttreeIndex = InRun;
    fillBranches(InRun, r, ttreeIndex);
    runTree_.optimizeBaskets(10ULL * 1024 * 1024);

    Service<JobReport> reportSvc;
    reportSvc->reportRunNumber(reportToken_, r.run());
  }

  void RootOutputFile::writeProcessBlock(ProcessBlockForOutput const& pb) {
    std::string const& processName = pb.processName();
    std::vector<std::string> const& processesWithProcessBlockProducts =
        om_->outputProcessBlockHelper().processesWithProcessBlockProducts();
    std::vector<std::string>::const_iterator it =
        std::find(processesWithProcessBlockProducts.cbegin(), processesWithProcessBlockProducts.cend(), processName);
    if (it == processesWithProcessBlockProducts.cend()) {
      return;
    }
    unsigned int ttreeIndex = InProcess + std::distance(processesWithProcessBlockProducts.cbegin(), it);
    fillBranches(InProcess, pb, ttreeIndex);
    treePointers_[ttreeIndex]->optimizeBaskets(10ULL * 1024 * 1024);
  }

  void RootOutputFile::writeParentageRegistry() {
    Parentage const* desc(nullptr);

    if (!parentageTree_->Branch(poolNames::parentageBranchName().c_str(), &desc, om_->basketSize(), 0))
      throw Exception(errors::FatalRootError) << "Failed to create a branch for Parentages in the output file";

    ParentageRegistry& ptReg = *ParentageRegistry::instance();

    std::vector<ParentageID> orderedIDs(parentageIDs_.size());
    for (auto const& parentageID : parentageIDs_) {
      orderedIDs[parentageID.second] = parentageID.first;
    }
    //now put them into the TTree in the correct order
    for (auto const& orderedID : orderedIDs) {
      desc = ptReg.getMapped(orderedID);
      //NOTE: some old format files have missing Parentage info
      // so a null value of desc can't be fatal.
      // Root will default construct an object in that case.
      parentageTree_->Fill();
    }
  }

  void RootOutputFile::writeFileFormatVersion() {
    FileFormatVersion fileFormatVersion(getFileFormatVersion());
    FileFormatVersion* pFileFmtVsn = &fileFormatVersion;
    TBranch* b =
        metaDataTree_->Branch(poolNames::fileFormatVersionBranchName().c_str(), &pFileFmtVsn, om_->basketSize(), 0);
    assert(b);
    b->Fill();
  }

  void RootOutputFile::writeFileIdentifier() {
    FileID* fidPtr = &fid_;
    TBranch* b = metaDataTree_->Branch(poolNames::fileIdentifierBranchName().c_str(), &fidPtr, om_->basketSize(), 0);
    assert(b);
    b->Fill();
  }

  void RootOutputFile::writeIndexIntoFile() {
    if (eventTree_.checkEntriesInReadBranches(eventEntryNumber_) == false) {
      Exception ex(errors::OtherCMS);
      ex << "The number of entries in at least one output TBranch whose entries\n"
            "were copied from the input does not match the number of events\n"
            "recorded in IndexIntoFile. This might (or might not) indicate a\n"
            "problem related to fast copy.";
      ex.addContext("Calling RootOutputFile::writeIndexIntoFile");
      throw ex;
    }
    indexIntoFile_.sortVector_Run_Or_Lumi_Entries();
    IndexIntoFile* iifPtr = &indexIntoFile_;
    TBranch* b = metaDataTree_->Branch(poolNames::indexIntoFileBranchName().c_str(), &iifPtr, om_->basketSize(), 0);
    assert(b);
    b->Fill();
  }

  void RootOutputFile::writeStoredMergeableRunProductMetadata() {
    storedMergeableRunProductMetadata_.optimizeBeforeWrite();
    StoredMergeableRunProductMetadata* ptr = &storedMergeableRunProductMetadata_;
    TBranch* b =
        metaDataTree_->Branch(poolNames::mergeableRunProductMetadataBranchName().c_str(), &ptr, om_->basketSize(), 0);
    assert(b);
    b->Fill();
  }

  void RootOutputFile::writeProcessHistoryRegistry() {
    fillProcessHistoryBranch(metaDataTree_.get(), om_->basketSize(), processHistoryRegistry_);
  }

  void RootOutputFile::writeBranchIDListRegistry() {
    BranchIDLists const* p = om_->branchIDLists();
    TBranch* b = metaDataTree_->Branch(poolNames::branchIDListBranchName().c_str(), &p, om_->basketSize(), 0);
    assert(b);
    b->Fill();
  }

  void RootOutputFile::writeThinnedAssociationsHelper() {
    ThinnedAssociationsHelper const* p = om_->thinnedAssociationsHelper();
    TBranch* b =
        metaDataTree_->Branch(poolNames::thinnedAssociationsHelperBranchName().c_str(), &p, om_->basketSize(), 0);
    assert(b);
    b->Fill();
  }

  void RootOutputFile::writeParameterSetRegistry() {
    fillParameterSetBranch(parameterSetsTree_.get(), om_->basketSize());
  }

  void RootOutputFile::writeProductDescriptionRegistry(ProductRegistry const& iReg) {
    // Make a local copy of the ProductRegistry, removing any transient or pruned products.
    using ProductList = ProductRegistry::ProductList;
    ProductRegistry pReg(iReg.productList());
    ProductList& pList = const_cast<ProductList&>(pReg.productList());
    for (auto const& prod : pList) {
      if (prod.second.branchID() != prod.second.originalBranchID()) {
        if (branchesWithStoredHistory_.find(prod.second.branchID()) != branchesWithStoredHistory_.end()) {
          branchesWithStoredHistory_.insert(prod.second.originalBranchID());
        }
      }
    }
    std::set<BranchID>::iterator end = branchesWithStoredHistory_.end();
    for (ProductList::iterator it = pList.begin(); it != pList.end();) {
      if (branchesWithStoredHistory_.find(it->second.branchID()) == end) {
        // avoid invalidating iterator on deletion
        ProductList::iterator itCopy = it;
        ++it;
        pList.erase(itCopy);

      } else {
        ++it;
      }
    }

    ProductRegistry* ppReg = &pReg;
    TBranch* b = metaDataTree_->Branch(poolNames::productDescriptionBranchName().c_str(), &ppReg, om_->basketSize(), 0);
    assert(b);
    b->Fill();
  }
  void RootOutputFile::writeProductDependencies() {
    ProductDependencies& pDeps = const_cast<ProductDependencies&>(om_->productDependencies());
    ProductDependencies* ppDeps = &pDeps;
    TBranch* b =
        metaDataTree_->Branch(poolNames::productDependenciesBranchName().c_str(), &ppDeps, om_->basketSize(), 0);
    assert(b);
    b->Fill();
  }

  // For duplicate removal and to determine if fast cloning is possible, the input
  // module by default reads the entire EventAuxiliary branch when it opens the
  // input files.  If EventAuxiliary is written in the usual way, this results
  // in many small reads scattered throughout the file, which can have very poor
  // performance characteristics on some filesystems.  As a workaround, we save
  // EventAuxiliary and write it at the end of the file.

  void RootOutputFile::writeEventAuxiliary() {
    constexpr std::size_t maxEaBasketSize = 4 * 1024 * 1024;

    if (om_->compactEventAuxiliary()) {
      auto tree = eventTree_.tree();
      auto const& bname = BranchTypeToAuxiliaryBranchName(InEvent).c_str();

      tree->SetBranchStatus(bname, true);
      auto basketsize =
          std::min(maxEaBasketSize,
                   compactEventAuxiliary_.size() * (sizeof(EventAuxiliary) + 26));  // 26 is an empirical fudge factor
      tree->SetBasketSize(bname, basketsize);
      auto b = tree->GetBranch(bname);

      assert(b);

      LogDebug("writeEventAuxiliary") << "EventAuxiliary ratio extras/GUIDs/all = "
                                      << compactEventAuxiliary_.extrasSize() << "/"
                                      << compactEventAuxiliary_.guidsSize() << "/" << compactEventAuxiliary_.size();

      for (auto const& aux : compactEventAuxiliary_) {
        const auto ea = aux.eventAuxiliary();
        pEventAux_ = &ea;
        // Fill EventAuxiliary branch
        b->Fill();
      }
      eventTree_.setEntries();
    }
  }

  void RootOutputFile::writeProcessBlockHelper() {
    if (!om_->outputProcessBlockHelper().processesWithProcessBlockProducts().empty()) {
      StoredProcessBlockHelper storedProcessBlockHelper(
          om_->outputProcessBlockHelper().processesWithProcessBlockProducts());
      om_->outputProcessBlockHelper().fillCacheIndices(storedProcessBlockHelper);

      StoredProcessBlockHelper* pStoredProcessBlockHelper = &storedProcessBlockHelper;
      TBranch* b = metaDataTree_->Branch(
          poolNames::processBlockHelperBranchName().c_str(), &pStoredProcessBlockHelper, om_->basketSize(), 0);
      assert(b);
      b->Fill();
    }
  }

  void RootOutputFile::finishEndFile() {
    std::string_view status = "beginning";
    std::string_view value = "";
    try {
      metaDataTree_->SetEntries(-1);
      status = "writeTTree() for metadata";
      RootOutputTree::writeTTree(metaDataTree_);
      status = "writeTTree() for ParameterSets";
      RootOutputTree::writeTTree(parameterSetsTree_);

      status = "writeTTree() for parentage";
      RootOutputTree::writeTTree(parentageTree_);

      // Create branch aliases for all the branches in the
      // events/lumis/runs/processblock trees. The loop is over
      // all types of data products.
      status = "writeTree() for ";
      for (unsigned int i = 0; i < treePointers_.size(); ++i) {
        std::string processName;
        BranchType branchType = InProcess;
        if (i < InProcess) {
          branchType = static_cast<BranchType>(i);
        } else {
          processName = om_->outputProcessBlockHelper().processesWithProcessBlockProducts()[i - InProcess];
        }
        setBranchAliases(treePointers_[i]->tree(), om_->keptProducts()[branchType], processName);
        value = treePointers_[i]->tree()->GetName();
        treePointers_[i]->writeTree();
      }

      // close the file -- mfp
      // Just to play it safe, zero all pointers to objects in the TFile to be closed.
      status = "closing TTrees";
      value = "";
      metaDataTree_ = parentageTree_ = nullptr;
      for (auto& treePointer : treePointers_) {
        treePointer->close();
        treePointer = nullptr;
      }
      status = "closing TFile";
      filePtr_->Close();
      filePtr_ = nullptr;  // propagate_const<T> has no reset() function

      // report that file has been closed
      status = "reporting to JobReport";
      Service<JobReport> reportSvc;
      reportSvc->outputFileClosed(reportToken_);
    } catch (cms::Exception& e) {
      e.addContext("Calling RootOutputFile::finishEndFile() while closing " + file_);
      e.addAdditionalInfo("While calling " + std::string(status) + std::string(value));
      throw;
    }
  }

  void RootOutputFile::setBranchAliases(TTree* tree,
                                        SelectedProducts const& branches,
                                        std::string const& processName) const {
    if (tree && tree->GetNbranches() != 0) {
      auto const& aliasForBranches = om_->aliasForBranches();
      for (auto const& selection : branches) {
        ProductDescription const& pd = *selection.first;
        if (pd.branchType() == InProcess && processName != pd.processName()) {
          continue;
        }
        std::string const& full = pd.branchName() + "obj";
        bool matched = false;
        for (auto const& matcher : aliasForBranches) {
          if (matcher.match(pd.branchName())) {
            tree->SetAlias(matcher.alias_.c_str(), full.c_str());
            matched = true;
          }
        }
        if (not matched and pd.branchAliases().empty()) {
          std::string const& alias = (pd.productInstanceName().empty() ? pd.moduleLabel() : pd.productInstanceName());
          tree->SetAlias(alias.c_str(), full.c_str());
        } else {
          for (auto const& alias : pd.branchAliases()) {
            tree->SetAlias(alias.c_str(), full.c_str());
          }
        }
      }
    }
  }

  void RootOutputFile::insertAncestors(ProductProvenance const& iGetParents,
                                       ProductProvenanceRetriever const* iMapper,
                                       bool produced,
                                       std::set<BranchID> const& iProducedIDs,
                                       std::set<StoredProductProvenance>& oToFill) {
    assert(om_->dropMetaData() != PoolOutputModule::DropAll);
    assert(produced || om_->dropMetaData() != PoolOutputModule::DropPrior);
    if (om_->dropMetaData() == PoolOutputModule::DropDroppedPrior && !produced)
      return;
    std::vector<BranchID> const& parentIDs = iGetParents.parentage().parents();
    for (auto const& parentID : parentIDs) {
      branchesWithStoredHistory_.insert(parentID);
      ProductProvenance const* info = iMapper->branchIDToProvenance(parentID);
      if (info) {
        if (om_->dropMetaData() == PoolOutputModule::DropNone ||
            (iProducedIDs.end() != iProducedIDs.find(info->branchID()))) {
          if (insertProductProvenance(*info, oToFill)) {
            //haven't seen this one yet
            insertAncestors(*info, iMapper, produced, iProducedIDs, oToFill);
          }
        }
      }
    }
  }

  void RootOutputFile::fillBranches(BranchType const& branchType,
                                    OccurrenceForOutput const& occurrence,
                                    unsigned int ttreeIndex,
                                    StoredProductProvenanceVector* productProvenanceVecPtr,
                                    ProductProvenanceRetriever const* provRetriever) {
    std::vector<std::unique_ptr<WrapperBase> > dummies;

    OutputItemList& items = om_->selectedOutputItemList()[ttreeIndex];

    bool const doProvenance =
        (productProvenanceVecPtr != nullptr) && (om_->dropMetaData() != PoolOutputModule::DropAll);
    bool const keepProvenanceForPrior = doProvenance && om_->dropMetaData() != PoolOutputModule::DropPrior;

    bool const fastCloning = (branchType == InEvent) && (whyNotFastClonable_ == FileBlock::CanFastClone);
    std::set<StoredProductProvenance> provenanceToKeep;
    //
    //If we are dropping some of the meta data we need to know
    // which BranchIDs were produced in this process because
    // we may be storing meta data for only those products
    // We do this only for event products.
    std::set<BranchID> producedBranches;
    if (doProvenance && branchType == InEvent && om_->dropMetaData() != PoolOutputModule::DropNone) {
      for (auto bd : occurrence.productRegistry().allProductDescriptions()) {
        if (bd->produced() && bd->branchType() == InEvent) {
          producedBranches.insert(bd->branchID());
        }
      }
    }

    // Loop over EDProduct branches, possibly fill the provenance, and write the branch.
    for (auto& item : items) {
      BranchID const& id = item.productDescription()->branchID();
      branchesWithStoredHistory_.insert(id);

      bool produced = item.productDescription()->produced();
      bool getProd =
          (produced || !fastCloning || treePointers_[ttreeIndex]->uncloned(item.productDescription()->branchName()));
      bool keepProvenance = doProvenance && (produced || keepProvenanceForPrior);

      WrapperBase const* product = nullptr;
      ProductProvenance const* productProvenance = nullptr;
      if (getProd) {
        BasicHandle result = occurrence.getByToken(item.token(), item.productDescription()->unwrappedTypeID());
        product = result.wrapper();
        if (result.isValid() && keepProvenance) {
          productProvenance = result.provenance()->productProvenance();
        }
        if (product == nullptr) {
          // No product with this ID is in the event.
          // Add a null product.
          TClass* cp = item.productDescription()->wrappedType().getClass();
          assert(cp != nullptr);
          int offset = cp->GetBaseClassOffset(wrapperBaseTClass_);
          void* p = cp->New();
          std::unique_ptr<WrapperBase> dummy = getWrapperBasePtr(p, offset);
          product = dummy.get();
          dummies.emplace_back(std::move(dummy));
        }
        item.setProduct(product);
      }
      if (keepProvenance && productProvenance == nullptr) {
        productProvenance = provRetriever->branchIDToProvenance(item.productDescription()->originalBranchID());
      }
      if (productProvenance) {
        insertProductProvenance(*productProvenance, provenanceToKeep);
        insertAncestors(*productProvenance, provRetriever, produced, producedBranches, provenanceToKeep);
      }
    }

    if (doProvenance)
      productProvenanceVecPtr->assign(provenanceToKeep.begin(), provenanceToKeep.end());
    treePointers_[ttreeIndex]->fillTree();
    if (doProvenance)
      productProvenanceVecPtr->clear();
  }

  bool RootOutputFile::insertProductProvenance(const edm::ProductProvenance& iProv,
                                               std::set<edm::StoredProductProvenance>& oToInsert) {
    StoredProductProvenance toStore;
    toStore.branchID_ = iProv.branchID().id();
    std::set<edm::StoredProductProvenance>::iterator itFound = oToInsert.find(toStore);
    if (itFound == oToInsert.end()) {
      //get the index to the ParentageID or insert a new value if not already present
      std::pair<std::map<edm::ParentageID, unsigned int>::iterator, bool> i =
          parentageIDs_.insert(std::make_pair(iProv.parentageID(), static_cast<unsigned int>(parentageIDs_.size())));
      toStore.parentageIDIndex_ = i.first->second;
      if (toStore.parentageIDIndex_ >= parentageIDs_.size()) {
        throw edm::Exception(errors::LogicError)
            << "RootOutputFile::insertProductProvenance\n"
            << "The parentage ID index value " << toStore.parentageIDIndex_
            << " is out of bounds.  The maximum value is currently " << parentageIDs_.size() - 1 << ".\n"
            << "This should never happen.\n"
            << "Please report this to the framework developers.";
      }

      oToInsert.insert(toStore);
      return true;
    }
    return false;
  }
}  // namespace edm
