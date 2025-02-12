#include "NuHepMCInputHandler.h"

#include "HepMC3/Print.h"
#include "HepMC3/ReaderFactory.h"

#include <stdexcept>

namespace NuHepMC {
namespace VertexStatus {
const int kPrimaryVertex = 1;
const int kNuclearVertex = 2;
} // namespace VertexStatus

namespace ParticleStatus {
const int kUndecayedPhysicalParticle = 1;
const int kIncomingBeamParticle = 4;
const int kTargetParticle = 11;
const int kStruckNucleon = 21;
} // namespace ParticleStatus

} // namespace NuHepMC

NuHepMCInputHandler::~NuHepMCInputHandler(){

};

namespace NuHepMC {
template <typename T> bool HasAttribute(T const &obj, std::string const &name) {
  auto attr_names = obj->attribute_names();
  return std::find(attr_names.begin(), attr_names.end(), name) !=
         attr_names.end();
}

template <typename AT, typename T>
auto CheckedAttributeValue(T const &obj, std::string const &name) {
  if (!obj) {
    throw std::runtime_error("CheckedAttributeValue");
  }

  if (!HasAttribute(obj, name)) {
    std::cout << "[ERROR]: Failed to find attribute: " << name;
    std::cout << "\n\tKnown attributes: \n";
    for (auto const &a : obj->attribute_names()) {
      std::cout << "\t\t" << a << "\n";
    }
    throw std::runtime_error("CheckedAttributeValue");
  }

  if (!obj->template attribute<AT>(name)) {
    std::cout << "[ERROR]: " << name << ": " << obj->attribute_as_string(name);
    throw std::runtime_error("CheckedAttributeValue");
  }

  return obj->template attribute<AT>(name)->value();
}

template <typename AT, typename T>
auto CheckedAttributeValue(T const &obj, std::string const &name,
                           decltype(obj->template attribute<AT>(name)->value())
                               const defval) {
  if (!obj) {
    throw std::runtime_error("CheckedAttributeValue");
  }

  if (!HasAttribute(obj, name)) {
    return defval;
  }

  if (!obj->template attribute<AT>(name)) {
    std::cout << "[ERROR]: " << name << ": " << obj->attribute_as_string(name);
    throw std::runtime_error("CheckedAttributeValue");
  }

  return obj->template attribute<AT>(name)->value();
}

bool SignalsConvention(std::shared_ptr<HepMC3::GenRunInfo> const &run_info,
                       std::string const &Convention) {
  auto conventions =
      NuHepMC::CheckedAttributeValue<HepMC3::VectorStringAttribute>(
          run_info, "NuHepMC.Conventions");

  for (auto const &c : conventions) {
    if (c == Convention) {
      return true;
    }
  }
  return false;
}

} // namespace NuHepMC

NuHepMCInputHandler::NuHepMCInputHandler(std::string const &handle,
                                         std::string const &rawinputs)
    : frun_info(nullptr) {

  NUIS_LOG(SAM, "Creating NuHepMCInputHandler : " << handle);

  // Run a joint input handling
  fName = handle;
  jointinput = false;
  jointindexswitch = 0;

  // Get initial flags
  fMaxEvents = FitPar::Config().GetParI("MAXEVENTS");

  // Form list of all inputs, remove brackets if required.
  std::vector<std::string> inputs = GeneralUtils::ParseToStr(rawinputs, ",");
  if (inputs.front()[0] == '(') {
    inputs.front() = inputs.front().substr(1);
  }
  if (inputs.back()[inputs.back().size() - 1] == ')') {
    inputs.back() = inputs.back().substr(0, inputs.back().size() - 1);
  }
  for (size_t inp_it = 0; inp_it < inputs.size(); ++inp_it) {
    if (LOG_LEVEL(SAM)) {
      std::cout << "\t\t|-> Input File " << inp_it << "      : "
                << inputs[inp_it] << std::endl;
    }
  }

  fEventType = kNuHepMC;

  fFilename = inputs[0];

  fReader = HepMC3::deduce_reader(fFilename);

  bool has_running_xsec_estimate = false;
  double best_xs_estimate = 0;
  bool has_FATX = false;
  double to_1em38_cm2 = 1; // Conversion factor to the typical 10^-38 cm^2 units used
  size_t NWeights = 0;
  // Loop through events and get N
  fNEvents = 0;
  double sum_of_weights = 0;
  while (!fReader->failed()) {
    fReader->read_event(fHepMC3Evt);
    if (!fReader->failed()) {
      // std::cout << "read event: " << fNEvents << std::endl;
      fNEvents++;
    } else {
      // std::cout << "reader failed after " << fNEvents << " events."
      //           << std::endl;
      break;
    }

    if (!frun_info) {
      frun_info = fReader->run_info();

      has_FATX = NuHepMC::SignalsConvention(frun_info, "G.C.4");
      has_running_xsec_estimate =
          NuHepMC::SignalsConvention(frun_info, "E.C.4");
      if(NuHepMC::SignalsConvention(frun_info, "E.C.5")) 
          to_1em38_cm2 = 1e2;

      std::cout << "Input file contains weights:" << std::endl;
      for (auto const &wn : frun_info->weight_names()) {
        std::cout << "\t" << wn << std::endl;
        NWeights++;
      }
    }

    if (has_running_xsec_estimate) {
      auto xs = fHepMC3Evt.cross_section();
      if (xs) {
        if (NWeights > 0) {
          best_xs_estimate = xs->xsecs()[0];
          sum_of_weights += fHepMC3Evt.weights()[0];
          std::cout << "xs:xsec()[0] = " << xs->xsecs()[0]
                    << ", weights()[0] = " << fHepMC3Evt.weights()[0]
                    << std::endl;
        }
      } else {
        std::cout << "[WARN]: Failed to read xs info for " << (fNEvents - 1)
                  << std::endl;
      }
    }
  }

  std::cout << "sum of weights = " << sum_of_weights << std::endl;
  std::cout << "nevents = " << fNEvents << std::endl;
  std::cout << "best_xs_estimate = " << best_xs_estimate << std::endl;

  // Open the file again
  fReader = HepMC3::deduce_reader(fFilename);
  nextentry = 0;

  if (!frun_info) {
    NUIS_ABORT(
        "Could not read run_info from input NuHepMC file: " << fFilename);
  }

  double fatx_cm2 = 1;

  if (has_FATX) {
    double FATX = NuHepMC::CheckedAttributeValue<HepMC3::DoubleAttribute>(
        frun_info, "NuHepMC.FluxAveragedTotalCrossSection");
    fatx_cm2 = FATX*to_1em38_cm2; // back into 1e-38 cm2
  } else if (has_running_xsec_estimate) {
    fatx_cm2 = best_xs_estimate*to_1em38_cm2; // back into 1e-38 cm2
  }

  // Dupe the FATX
  fEventHist = new TH1D("eventhist", "eventhist", 10, 0.0, 10.0);
  fEventHist->SetBinContent(5, fatx_cm2);
  fFluxHist = new TH1D("fluxhist", "fluxhist", 10, 0.0, 10.0);
  fFluxHist->SetBinContent(5, 1);

  fNUISANCEEvent = new FitEvent();
  fNUISANCEEvent->HardReset();
  fBaseEvent = static_cast<BaseFitEvt *>(fNUISANCEEvent);
};

FitEvent *NuHepMCInputHandler::GetNuisanceEvent(const UInt_t entry, bool) {

  int ntoskip = 0;

  if (nextentry != entry) {
    if (nextentry > entry) {
      // start the file again
      fReader = HepMC3::deduce_reader(fFilename);
      ntoskip = entry;
    } else {
      ntoskip = entry - nextentry;
    }
  }

  if (ntoskip) {
    fReader->skip(ntoskip);
  }

  nextentry = entry + 1;

  fReader->read_event(fHepMC3Evt);

  // Catch too large entries
  if (fReader->failed()) {
    return NULL;
  }

  // Setup Input scaling for joint inputs
  if (jointinput) {
    fNUISANCEEvent->InputWeight = GetInputWeight(entry);
  } else {
    fNUISANCEEvent->InputWeight = 1.0;
  }

  // Run NUISANCE Vector Filler
  CalcNUISANCEKinematics();

  // Return event pointer
  return fNUISANCEEvent;
}

void NuHepMCInputHandler::CalcNUISANCEKinematics() {

  // Reset all variables
  fNUISANCEEvent->ResetEvent();

  fNUISANCEEvent->Mode =
      fHepMC3Evt.attribute<HepMC3::IntAttribute>("ProcID")->value();

  fNUISANCEEvent->fEventNo = fHepMC3Evt.event_number();

  // Read all particles from fHepMCEvent
  fNUISANCEEvent->fNParticles = 0;
  for (auto const &p : fHepMC3Evt.particles()) {

    int status = 0;

    if (p->status() == NuHepMC::ParticleStatus::kIncomingBeamParticle) {
      status = kInitialState;
    } else if (p->status() == NuHepMC::ParticleStatus::kTargetParticle) {

      status = kNuclearInitial;

      fNUISANCEEvent->fTargetA = (p->pid() / 10) % 1000;
      fNUISANCEEvent->fTargetZ = (p->pid() / 10000) % 1000;
      fNUISANCEEvent->fTargetH = 0;
      fNUISANCEEvent->fBound = (p->pid() == 1000010010);

    } else if (p->status() == NuHepMC::ParticleStatus::kStruckNucleon) {
      status = kInitialState;
    } else if (p->status() ==
               NuHepMC::ParticleStatus::kUndecayedPhysicalParticle) {
      status = kFinalState;
    } else {
      // Ignore internal particles
      continue;
    }

    fNUISANCEEvent->fPrimaryVertex[fNUISANCEEvent->fNParticles] =
        (p->production_vertex()->status() ==
         NuHepMC::VertexStatus::kPrimaryVertex);

    // Mom
    fNUISANCEEvent->fParticleMom[fNUISANCEEvent->fNParticles][0] =
        p->momentum().px();
    fNUISANCEEvent->fParticleMom[fNUISANCEEvent->fNParticles][1] =
        p->momentum().py();
    fNUISANCEEvent->fParticleMom[fNUISANCEEvent->fNParticles][2] =
        p->momentum().pz();
    fNUISANCEEvent->fParticleMom[fNUISANCEEvent->fNParticles][3] =
        p->momentum().e();

    // PDG
    fNUISANCEEvent->fParticlePDG[fNUISANCEEvent->fNParticles] = p->pid();
    fNUISANCEEvent->fParticleState[fNUISANCEEvent->fNParticles] = status;

    // Add up particle count
    fNUISANCEEvent->fNParticles++;
  }

  // Run Initial, FSI, Final, Other ordering.
  fNUISANCEEvent->OrderStack();

  return;
}

double NuHepMCInputHandler::GetInputWeight(const UInt_t entry) {

  // Find Switch Scale
  while (entry < (UInt_t)jointindexlow[jointindexswitch] ||
         entry >= (UInt_t)jointindexhigh[jointindexswitch]) {
    jointindexswitch++;

    // Loop Around
    if (jointindexswitch == jointindexlow.size()) {
      jointindexswitch = 0;
    }
  }
  return jointindexscale[jointindexswitch];
};

BaseFitEvt *NuHepMCInputHandler::GetBaseEvent(const UInt_t entry) {

  if (entry >= (UInt_t)fNEvents)
    return NULL;
  return (BaseFitEvt *)GetNuisanceEvent(entry, true);
}
