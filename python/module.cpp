/*******************************************************************************

  Copyright (c) Honda Research Institute Europe GmbH

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.

  3. Neither the name of the copyright holder nor the names of its
     contributors may be used to endorse or promote products derived from
     this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*******************************************************************************/

#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include "pybind11_json.hpp"
#include "pybind_dict_utils.h"
#include "type_casters.h"

namespace py = pybind11;

#include <LandmarkBase.h>
#include <ExampleActionsECS.h>
#include <ActionFactory.h>
#include <ActionSequence.h>
#include <HardwareComponent.h>
#include <LandmarkZmqComponent.h>
#include <TTSComponent.h>
#include <PredictionTree.h>
#include <AzureSkeletonTracker.h>
#include "ActionEyeGaze.h"

#include <Rcs_resourcePath.h>
#include <Rcs_macros.h>
#include <Rcs_math.h>
#include <Rcs_timer.h>
#include <Rcs_typedef.h>
#include <Rcs_utilsCPP.h>
#include <json.hpp>

#include <SegFaultHandler.h>

#if !defined(_MSC_VER)
#include <X11/Xlib.h>
#endif

#include <chrono>
#include <vector>
#include <tuple>

RCS_INSTALL_ERRORHANDLERS



//////////////////////////////////////////////////////////////////////////////
// Fully threaded tree prediction and execution function.
//////////////////////////////////////////////////////////////////////////////
void _planActionSequenceThreaded(aff::ExampleActionsECS& ex,
                                 std::string sequenceCommand,
                                 size_t maxNumThreads)
{
  std::string errMsg;
  std::vector<std::string> seq = Rcs::String_split(sequenceCommand, ";");
  auto tree = ex.getQuery()->planActionTree(aff::PredictionTree::SearchType::DFSMT,
                                            seq, ex.getEntity().getDt(), maxNumThreads);
  auto res = tree->findSolutionPathAsStrings();

  if (res.empty())
  {
    RLOG_CPP(0, "Could not find solution");
    ex.setProcessingAction(false);
    ex.clearCompletedActionStack();
    return;
  }

  RLOG_CPP(0, "Sequence has " << res.size() << " steps");

  std::string newCmd;
  for (size_t i = 0; i < res.size(); ++i)
  {
    newCmd += res[i];
    newCmd += ";";
  }

  RLOG_CPP(0, "Command : " << newCmd);
  ex.getEntity().publish("ActionSequence", newCmd);
}

//////////////////////////////////////////////////////////////////////////////
// Simple helper class that blocks the execution in the wait() function
// until the ActionResult event has been received.
//////////////////////////////////////////////////////////////////////////////
class PollBlockerComponent
{
public:

  PollBlockerComponent(aff::ExampleActionsECS* sim_) : sim(sim_)
  {
    sim->setProcessingAction(true);
  }

  void wait()
  {
    while (sim->isProcessingAction())
    {
      Timer_waitDT(0.1);
    }
    RLOG(0, "Done wait");
  }

  aff::ExampleActionsECS* sim;
};





//////////////////////////////////////////////////////////////////////////////
// Affordance enums for convenience in the python world
//////////////////////////////////////////////////////////////////////////////
void define_AffordanceTypes(py::module& m)
{
  py::enum_<aff::Affordance::Type>(m, "AffordanceType")
  .value("Affordance", aff::Affordance::Type::Affordance)
  .value("Graspable", aff::Affordance::Type::Graspable)
  .value("PowerGraspable", aff::Affordance::Type::PowerGraspable)
  .value("PincerGraspable", aff::Affordance::Type::PincerGraspable)
  .value("PalmGraspable", aff::Affordance::Type::PalmGraspable)
  .value("BallGraspable", aff::Affordance::Type::BallGraspable)
  .value("CircularGraspable", aff::Affordance::Type::CircularGraspable)
  .value("TwistGraspable", aff::Affordance::Type::TwistGraspable)
  .value("Twistable", aff::Affordance::Type::Twistable)
  .value("PushSwitchable", aff::Affordance::Type::PushSwitchable)
  .value("Supportable", aff::Affordance::Type::Supportable)
  .value("Stackable", aff::Affordance::Type::Stackable)
  .value("Containable", aff::Affordance::Type::Containable)
  .value("Pourable", aff::Affordance::Type::Pourable)
  .value("PointPushable", aff::Affordance::Type::PointPushable)
  .value("PointPokable", aff::Affordance::Type::PointPokable)
  .value("Hingeable", aff::Affordance::Type::Hingeable)
  .value("Dispensible", aff::Affordance::Type::Dispensible)
  .value("Wettable", aff::Affordance::Type::Wettable)
  .value("Openable", aff::Affordance::Type::Openable);
}





//////////////////////////////////////////////////////////////////////////////
// The python affaction module, mainly consisting off the LlmSim class.
//////////////////////////////////////////////////////////////////////////////
PYBIND11_MODULE(pyAffaction, m)
{
  define_AffordanceTypes(m);

  //////////////////////////////////////////////////////////////////////////////
  // LlmSim constructor
  //////////////////////////////////////////////////////////////////////////////
  py::class_<aff::ExampleActionsECS>(m, "LlmSim")
  .def(py::init<>([]()
  {
#if !defined(_MSC_VER)// Avoid crashes when running remotely.
    static bool xInitialized = false;
    if (!xInitialized)
    {
      xInitialized = true;
      XInitThreads();
    }
#endif

    auto ex = std::unique_ptr<aff::ExampleActionsECS>(new aff::ExampleActionsECS());
    ex->initParameters();
    return std::move(ex);
  }))

  //////////////////////////////////////////////////////////////////////////////
  // Initialization function, to be called after member variables have been
  // configured.
  //////////////////////////////////////////////////////////////////////////////
  .def("init", [](aff::ExampleActionsECS& ex, bool debug=false) -> bool
  {
    bool success = ex.initAlgo();

    if (debug)
    {
      success = ex.initGraphics() && success;
      ex.getEntity().publish("Render");
      ex.getEntity().process();
    }

    std::string starLine(80, '*');
    std::cerr << "\n\n" + starLine;
    if (success)
    {
      std::cerr << "\n* LLMSim initialized\n";
    }
    else
    {
      std::cerr << "\n* Failed to initialize LLMSim\n";
    }
    std::cerr << starLine << "\n";

    return success;
  }, "Initializes algorithm, guis and graphics")

  //////////////////////////////////////////////////////////////////////////////
  // Update one LlmSim instance from anotuer one
  //////////////////////////////////////////////////////////////////////////////
  .def("sync", [](aff::ExampleActionsECS& ex, py::object obj)
  {
    aff::ExampleActionsECS* sim = obj.cast<aff::ExampleActionsECS*>();
    RcsGraph_copy(ex.getGraph(), sim->getGraph());
    ex.getScene()->agents = sim->getScene()->agents;
    ex.step();
  })

  //////////////////////////////////////////////////////////////////////////////
  // Returns empty json if the agent can see all objects or a json in the form:
  // for the agent: {"occluded": [{"name": "entity name 1", "instance_id": "entity id 1"},
  //                              {"name": "entity name 2", "instance_id": "entity id 2"}]}
  //////////////////////////////////////////////////////////////////////////////
  .def("getOccludedObjectsForAgent", [](aff::ExampleActionsECS& ex, std::string agentName) -> nlohmann::json
  {
    return ex.getQuery()->getOccludedObjectsForAgent(agentName);
  })

  //////////////////////////////////////////////////////////////////////////////
  // Returns empty json if not occluded, or occluding objects sorted by distance
  // to eye (increasing): {"occluded_by": ["id_1", "id_2"] }
  //////////////////////////////////////////////////////////////////////////////
  .def("isOccludedBy", [](aff::ExampleActionsECS& ex, std::string agentName, std::string objectName) -> nlohmann::json
  {
    return ex.getQuery()->getObjectOccludersForAgent(agentName, objectName);
  })

  //////////////////////////////////////////////////////////////////////////////
  // Returns the position of the object in camera coordinates
  //////////////////////////////////////////////////////////////////////////////
  .def("getObjectInCamera", [](aff::ExampleActionsECS& ex, std::string objectName, std::string cameraName) -> nlohmann::json
  {
    return ex.getQuery()->getObjectInCamera(objectName, cameraName);
  })

  //////////////////////////////////////////////////////////////////////////////
  // Returns a boolean indicating if any scene entity is closer to any hand of
  // the agent closer than a distance threshold.
  //////////////////////////////////////////////////////////////////////////////
  .def("isBusy", [](aff::ExampleActionsECS& ex, std::string agentName) -> bool
  {
    return ex.getQuery()->isAgentBusy(agentName, 0.15);
  })

  //////////////////////////////////////////////////////////////////////////////
  // Returns the pan tilt angles for the agent when it looks at the gazeTarget
  //////////////////////////////////////////////////////////////////////////////
  .def("getPanTilt", [](aff::ExampleActionsECS& ex, std::string roboAgent, std::string gazeTarget)
  {
    RLOG(0, "Pan tilt angle calculation");

    if (!ex.getQuery())
    {
      RLOG(0, "panTiltQuery not yet constructed");
      double buf = 0.0;
      MatNd tmp = MatNd_fromPtr(0, 0, &buf);
      return pybind11::detail::MatNd_toNumpy(&tmp);
    }

    std::vector<double> panTilt = ex.getQuery()->getPanTilt(roboAgent, gazeTarget);

    if (panTilt.empty())
    {
      double buf = 0.0;
      MatNd tmp = MatNd_fromPtr(0, 0, &buf);
      return pybind11::detail::MatNd_toNumpy(&tmp);
    }

    MatNd tmp = MatNd_fromPtr(2, 1, panTilt.data());
    return pybind11::detail::MatNd_toNumpy(&tmp);
  })

  //////////////////////////////////////////////////////////////////////////////
  // Kinematic check of all agents if the object is within a reachable range.
  // For the robot agent: If the result is true, it does not necessarily mean
  // that it can be grasped
  //////////////////////////////////////////////////////////////////////////////
  .def("isReachable", [](aff::ExampleActionsECS& ex, std::string agentName, std::string objectName) -> bool
  {
    auto agent = ex.getScene()->getAgent(agentName);
    if (!agent)
    {
      RLOG_CPP(0, "Agent " << agentName << " unknown in scene. "
               << ex.getScene()->agents.size() << " agents:");
      for (const auto& agent : ex.getScene()->agents)
      {
        agent->print();
      }
      return false;
    }

    auto ntts = ex.getScene()->getAffordanceEntities(objectName);
    if (ntts.empty())
    {
      RLOG_CPP(0, "Object " << objectName << " unknown in scene");
      return false;
    }

    for (const auto& ntt : ntts)
    {
      const double* pos = ntt->body(ex.getGraph())->A_BI.org;
      if (agent->canReachTo(ex.getScene(), ex.getGraph(), pos))
      {
        return true;
      }
    }

    return false;
  }, "Check if agent can reach to the given position")

  .def("initHardwareComponents", [](aff::ExampleActionsECS& ex)
  {
    ex.getEntity().initialize(ex.getCurrentGraph());
  }, "Initializes hardware components")

  //////////////////////////////////////////////////////////////////////////////
  // Calls the run method in a new thread, releases the GIL and returns to the
  // python context (e.g. console).
  //////////////////////////////////////////////////////////////////////////////
  .def("run", &aff::ExampleActionsECS::startThreaded, py::call_guard<py::gil_scoped_release>(), "Starts endless loop")

  //////////////////////////////////////////////////////////////////////////////
  // Calls an event without arguments. We must not call process() here, since
  // this method might run concurrently to the event queue thread.
  //////////////////////////////////////////////////////////////////////////////
  .def("callEvent", [](aff::ExampleActionsECS& ex, std::string eventName)
  {
    ex.getEntity().publish(eventName);
  })
  //////////////////////////////////////////////////////////////////////////////
  // -1: grow down, 0: symmetric, 1: grow up
  // Returns number of changed shapes
  // Fatal error if body
  //////////////////////////////////////////////////////////////////////////////
  .def("changeShapeHeight", [](aff::ExampleActionsECS& ex, std::string nttName, double height, int growMode) -> size_t
  {
    auto ntts = ex.getScene()->getAffordanceEntities(nttName);
    RcsGraph* ikGraph = ex.getGraph();

    for (const auto& ntt : ntts)
    {
      const RcsBody* bdy = ntt->body(ikGraph);
      RCHECK_MSG(bdy->nShapes>0, "Body %s has no shapes attached", ntt->bdyName.c_str());
      const RcsShape* sh = &bdy->shapes[0];
      std::vector<double> newOrigin(sh->A_CB.org, sh->A_CB.org+3);
      newOrigin[2] += 0.5*growMode*(height-sh->extents[2]);

      ex.getEntity().publish("ChangeShapeHeight", ikGraph, ntt->bdyName, height);
      ex.getEntity().publish("ChangeShapeOrigin", ikGraph, ntt->bdyName, newOrigin);
    }

    return ntts.size();
  })
  .def("changeShapeDiameter", [](aff::ExampleActionsECS& ex, std::string nttName, double diameter) -> size_t
  {
    auto ntts = ex.getScene()->getAffordanceEntities(nttName);
    RcsGraph* ikGraph = ex.getGraph();

    for (const auto& ntt : ntts)
    {
      ex.getEntity().publish("ChangeShapeDiameter", ikGraph, ntt->bdyName, diameter);
    }

    return ntts.size();
  })
  .def("changeBodyOrigin", [](aff::ExampleActionsECS& ex, std::string bodyName, double x, double y, double z)
  {
    std::vector<double> org {x, y, z};
    RcsGraph* ikGraph = ex.getGraph();

    ex.getEntity().publish("ChangeBodyOrigin", ikGraph, bodyName, org);
  })
  .def("changeShapeOrigin", [](aff::ExampleActionsECS& ex, std::string bodyName, double x, double y, double z)
  {
    std::vector<double> org {x, y, z};
    RcsGraph* ikGraph = ex.getGraph();
    RLOG(1, "***");
    ex.getEntity().publish("ChangeShapeOrigin", ikGraph, bodyName, org);
  })
  .def("reset", [](aff::ExampleActionsECS& ex)
  {
    ex.getEntity().publish("ActionSequence", std::string("reset"));
  })
  .def("render", [](aff::ExampleActionsECS& ex)
  {
    ex.getEntity().publish("Render");
  })
  .def("process", [](aff::ExampleActionsECS& ex)
  {
    ex.getEntity().process();
  })
  .def("showGraphicsWindow", [](aff::ExampleActionsECS& ex) -> bool
  {
    bool success = ex.initGraphics();

    if (success)
    {
      ex.getEntity().publish("Render");
    }

    return success;
  })
  .def("showGuis", [](aff::ExampleActionsECS& ex) -> bool
  {
    return ex.initGuis();
  })

  .def("hideGraphicsWindow", [](aff::ExampleActionsECS& ex) -> bool
  {
    return ex.eraseViewer();
  })

  .def("get_state", [](aff::ExampleActionsECS& ex) -> std::string
  {
    return ex.getQuery()->getSceneState().dump();
  })

  //////////////////////////////////////////////////////////////////////////////
  // Returns the entire scene in a URDF format
  //////////////////////////////////////////////////////////////////////////////
  .def("get_state_urdf", [](aff::ExampleActionsECS& ex) -> std::string
  {
    return ex.getQuery()->getURDF();
  })

  //////////////////////////////////////////////////////////////////////////////
  // Returns a rendered image from the given coordinates
  //////////////////////////////////////////////////////////////////////////////
  .def("captureImage", [](aff::ExampleActionsECS& ex, double x, double y, double z,
                          double thx, double thy, double thz) -> std::tuple<py::array_t<double>, py::array_t<double>>
  {
    aff::VirtualCamera* virtualCamera = ex.getVirtualCamera();
    RCHECK_MSG(virtualCamera, "Virtual camera has not been instantiated");
    py::array_t<double> colorImage({virtualCamera->height, virtualCamera->width, 3}), depthImage({virtualCamera->height, virtualCamera->width});
    virtualCamera->render(x, y, z, thx, thy, thz, colorImage.mutable_data(), depthImage.mutable_data());
    return std::make_tuple(std::move(colorImage), std::move(depthImage));
  }, "Renders the current state of the scene. The input is the camera origin and yrp rotation around that origin. Outputs the color and depth image.")

  //////////////////////////////////////////////////////////////////////////////
  // Returns a rendered image from the given coordinates
  //////////////////////////////////////////////////////////////////////////////
  .def("captureColorImageFromFrame", [](aff::ExampleActionsECS& ex, std::string cameraName) -> py::array_t<double>
  {
    aff::VirtualCamera* virtualCamera = ex.getVirtualCamera();
    RCHECK_MSG(virtualCamera, "Virtual camera has not been instantiated");
    py::array_t<double> colorImage({virtualCamera->height, virtualCamera->width, 3});
    const RcsBody* cam = RcsGraph_getBodyByName(ex.getGraph(), cameraName.c_str());
    RCHECK(cam);

    virtualCamera->render(&cam->A_BI, colorImage.mutable_data(), nullptr);
    return std::move(colorImage);
  }, "Renders the current state of the scene. The input is the camera body name. Outputs the color image.")

  //////////////////////////////////////////////////////////////////////////////
  // Returns the entity of which child is a child of, or an empty string
  //////////////////////////////////////////////////////////////////////////////
  .def("get_parent_entity", [](aff::ExampleActionsECS& ex, std::string child) -> std::string
  {
    return ex.getQuery()->getParentEntity(child);
  })

  //////////////////////////////////////////////////////////////////////////////
  // Looks for the topoligical parent entity, and determines the closest frame
  // with the given type. Returns an empty string if none is found, or the name
  // of the affordance frame
  //////////////////////////////////////////////////////////////////////////////
  .def("get_closest_parent_affordance", [](aff::ExampleActionsECS& ex,
                                           std::string child, std::string affordanceType) -> std::string
  {
    return ex.getQuery()->getClosestParentAffordance(child, affordanceType);
  })

  //////////////////////////////////////////////////////////////////////////////
  // Returns empty json if there are no objects or a json in the form:
  // {"objects": ['iphone', 'red_glass', 'fanta_bottle'] }
  //////////////////////////////////////////////////////////////////////////////
  .def("get_objects", [](aff::ExampleActionsECS& ex) -> nlohmann::json
  {
    return ex.getQuery()->getObjects();
  })

  //////////////////////////////////////////////////////////////////////////////
  // Returns empty json if there are no objects or a json in the form:
  // {"agents": ['Daniel', 'Felix', 'Robot'] }
  //////////////////////////////////////////////////////////////////////////////
  .def("get_agents", [](aff::ExampleActionsECS& ex) -> nlohmann::json
  {
    return ex.getQuery()->getAgents();
  })

  //////////////////////////////////////////////////////////////////////////////
  // Returns an empty string if there are no objects held in the hand, or the
  // name of the holding hand
  //////////////////////////////////////////////////////////////////////////////
  .def("is_held_by", [](aff::ExampleActionsECS& ex, std::string ntt) -> std::string
  {
    return ex.getQuery()->getHoldingHand(ntt);
  })

  //////////////////////////////////////////////////////////////////////////////
  // Returns an empty string if there are no objects held in the hand, or the
  // name of the holding hand
  //////////////////////////////////////////////////////////////////////////////
  .def("get_objects_held_by", [](aff::ExampleActionsECS& ex, std::string agent) -> nlohmann::json
  {
    return ex.getQuery()->getObjectsHeldBy(agent);
  })

  //////////////////////////////////////////////////////////////////////////////
  // Execute the action command, and return immediately.
  //////////////////////////////////////////////////////////////////////////////
  //.def("execute", &aff::ExampleActionsECS::onActionSequence)
  .def("execute", [](aff::ExampleActionsECS& ex, std::string actionCommand)
  {
    ex.getEntity().publish("ActionSequence", actionCommand);
  })

  //////////////////////////////////////////////////////////////////////////////
  // Execute the action command, and return only after finished.
  //////////////////////////////////////////////////////////////////////////////
  .def("executeBlocking", [](aff::ExampleActionsECS& ex, std::string actionCommand) -> bool
  {
    PollBlockerComponent blocker(&ex);
    ex.getEntity().publish("ActionSequence", actionCommand);
    blocker.wait();
    RLOG_CPP(0, "Finished: " << actionCommand);
    bool success = ex.lastActionResult[0].success();

    RLOG(0, "   success=%s   result=%s", success ? "true" : "false", ex.lastActionResult[0].error.c_str());
    return success;
  })
  .def("getRobotCapabilities", [](aff::ExampleActionsECS& ex)
  {
    return aff::ActionFactory::printToString();
  })

  //////////////////////////////////////////////////////////////////////////////
  // Predict action sequence as tree
  // Call it like: agent.sim.predictActionSequence("get fanta_bottle;put fanta_bottle lego_box;")
  //////////////////////////////////////////////////////////////////////////////
  .def("predictActionSequence", [](aff::ExampleActionsECS& ex, std::string sequenceCommand) -> std::vector<std::string>
  {
    std::string errMsg;
    std::vector<std::string> seq = Rcs::String_split(sequenceCommand, ";");
    auto tree = ex.getQuery()->planActionTree(aff::PredictionTree::SearchType::DFSMT,
                                              seq, ex.getEntity().getDt());
    return tree ? tree->findSolutionPathAsStrings() : std::vector<std::string>();
    //return ex.getQuery()->planActionSequence(seq, seq.size());
  })

  //////////////////////////////////////////////////////////////////////////////
  // Predict action sequence as tree
  // Call it like: agent.sim.planActionSequence("get fanta_bottle;put fanta_bottle lego_box;")
  //////////////////////////////////////////////////////////////////////////////
  .def("planActionSequenceThreaded", [](aff::ExampleActionsECS& ex, std::string sequenceCommand)
  {
    ex.setProcessingAction(true);
    const size_t maxNumthreads = 0;   // 0 means auto-select
    std::thread t1(_planActionSequenceThreaded, std::ref(ex), sequenceCommand, maxNumthreads);
    t1.detach();
  })

  //////////////////////////////////////////////////////////////////////////////
  // Predict action sequence as tree
  // Call it like: agent.sim.planActionSequence("get fanta_bottle;put fanta_bottle lego_box;")
  //////////////////////////////////////////////////////////////////////////////
  .def("planActionSequence", [](aff::ExampleActionsECS& ex, std::string sequenceCommand, bool blocking) -> bool
  {
    std::vector<std::string> seq = Rcs::String_split(sequenceCommand, ";");
    std::string errMsg;

    auto tree = ex.getQuery()->planActionTree(aff::PredictionTree::SearchType::DFSMT,
                                              seq, ex.getEntity().getDt());
    auto res = tree->findSolutionPathAsStrings();

    if (res.empty())
    {
      RLOG_CPP(0, "Could not find solution");
      return false;
    }

    RLOG_CPP(0, "Sequence has " << res.size() << " steps");

    std::string newCmd;
    for (size_t i = 0; i < res.size(); ++i)
    {
      newCmd += res[i];
      newCmd += ";";
    }

    RLOG_CPP(0, "Command : " << newCmd);

    PollBlockerComponent blocker(&ex);
    ex.getEntity().publish("ActionSequence", newCmd);

    bool success = true;

    if (blocking)
    {
      blocker.wait();
      success = ex.lastActionResult[0].success();
      RLOG(0, "   success=%s   result=%s", success ? "true" : "false", ex.lastActionResult[0].error.c_str());
    }

    RLOG_CPP(0, "Finished: " << newCmd);

    return success;
  })

  //////////////////////////////////////////////////////////////////////////////
  // Predict action sequence as tree
  //////////////////////////////////////////////////////////////////////////////
  .def("plan_fb", [](aff::ExampleActionsECS& ex, std::string sequenceCommand) -> std::string
  {
    ex.getEntity().publish("FreezePerception", true);
    PollBlockerComponent blocker(&ex);
    ex.getEntity().publish("PlanDFSEE", sequenceCommand);
    blocker.wait();
    ex.getEntity().publish("FreezePerception", false);

    if (ex.lastActionResult[0].success())
    {
      RLOG_CPP(0, "SUCCESS");
      return "SUCCESS";
    }

    std::string fbmsgAsString = "No solution found:\n";
    std::string fbLine, fbLinePrev;
    for (size_t i = 0; i<ex.lastActionResult.size(); ++i)
    {
      const aff::ActionResult& fb = ex.lastActionResult[i];
      fbLine = fb.reason + " Suggestion: " + fb.suggestion + "\n";

      if (fbLine!=fbLinePrev)
      {
        fbmsgAsString += "  Issue " + std::to_string(i) + ": " + fbLine;
      }
      fbLinePrev = fbLine;
    }
    // size_t i = 0;
    // for (const auto& fb : ex.lastActionResult)
    // {
    //   fbmsgAsString += "  Issue " + std::to_string(i) + ": " + fb.reason + " Suggestion: " + fb.suggestion + "\n";
    //   ++i;
    // }

    RLOG_CPP(0, fbmsgAsString);

    return fbmsgAsString;
  })

  //////////////////////////////////////////////////////////////////////////////
  // Fill in the parameters of the action sequence,
  // providing rich information about the reason for failure
  //
  // return value: [(failure step, failure reason)] (empty if successful)
  //////////////////////////////////////////////////////////////////////////////
  .def("plan_fb_rich", [](aff::ExampleActionsECS& ex, std::string sequenceCommand) -> std::vector<std::tuple<std::vector<std::string>, std::string, std::string>>
  {
    const std::string actionSequence = aff::ActionSequence::resolve(ex.getGraph()->cfgFile, std::move(sequenceCommand));
    RLOG_CPP(0, "Processing sequence: '" << actionSequence << "'");
    std::vector<std::string> seq = Rcs::String_split(actionSequence, ";");

    auto tree = ex.getQuery()->planActionTree(aff::PredictionTree::SearchType::DFSMT, seq, ex.getEntity().getDt(),
                                              0, true, ex.earlyExitAction);

    // Handling a fatal error in the syntax for the first action
    RCHECK(tree);
    std::vector<aff::PredictionTreeNode*> slnPath = tree->findSolutionPath(0, false);
    if (slnPath.empty() || !tree->root->feedbackMsg.error.empty())
    {
      if (tree->root->fatalError)
      {
        RLOG_CPP(0, "Fatal Error in Solution 0");
      }
      return {std::make_tuple(std::vector<std::string>(), tree->root->feedbackMsg.reason, tree->root->feedbackMsg.suggestion)};
    }

    // Checking for whether the search was successful
    if (slnPath.back()->success && slnPath.size() == seq.size())
    {
      RLOG_CPP(0, "Solution 0 is SUCCESSFUL");

      std::vector<std::string> predictedSeq;
      predictedSeq.reserve(slnPath.size());
      for (auto node : slnPath)
      {
        predictedSeq.push_back(node->actionCommand());
      }

      std::string detailedActionCommand = Rcs::String_concatenate(predictedSeq, ";");
      RLOG_CPP(0, "Final action sequence: " + detailedActionCommand);

      PollBlockerComponent blocker(&ex);
      ex.getEntity().publish("ActionSequence", detailedActionCommand);
      blocker.wait();

      return std::vector<std::tuple<std::vector<std::string>, std::string, std::string>>();
    }

    // Checking the longest failure
    std::vector<aff::PredictionTreeNode*> leafs;
    tree->getLeafNodes(leafs, false);

    int deepest_level = 0;
    for (auto leaf : leafs)
    {
      deepest_level = std::max(deepest_level, leaf->level);
    }

    // Reporting only the nodes with the longest failure
    std::vector<aff::ActionResult> actionResults;
    std::vector<std::tuple<std::vector<std::string>, std::string, std::string>> searchResults;
    for (size_t slnIdx = 0; !slnPath.empty(); ++slnIdx, slnPath = tree->findSolutionPath(slnIdx, false))
    {
      RLOG_CPP(0, "Solution " << slnIdx << " is NOT SUCCESSFUL");
      if (slnPath.back()->level < deepest_level)
      {
        RLOG_CPP(0, "Solution " << slnIdx << " skipped");
        continue;
      }

      std::vector<std::string> predictedSeq;
      predictedSeq.reserve(slnPath.size());
      for (auto node : slnPath)
      {
        predictedSeq.push_back(node->actionCommand());
      }

      const aff::ActionResult& errMsg = slnPath.back()->feedbackMsg;
      actionResults.push_back(errMsg);
      searchResults.emplace_back(std::move(predictedSeq), errMsg.reason, errMsg.suggestion);
    }
    ex.getEntity().publish("ActionResult", false, 0.0, actionResults);
    return searchResults;
  })

  //////////////////////////////////////////////////////////////////////////////
  // Predict action sequence as tree, non-blocking version
  //////////////////////////////////////////////////////////////////////////////
  .def("plan_fb_nonblock", [](aff::ExampleActionsECS& ex, std::string sequenceCommand)
  {
    ex.setProcessingAction(true);
    ex.getEntity().publish("FreezePerception", true);
    ex.getEntity().publish("PlanDFSEE", sequenceCommand);
  })

  //////////////////////////////////////////////////////////////////////////////
  // Query non-blocking planner
  //////////////////////////////////////////////////////////////////////////////
  .def("query_fb_nonblock", [](aff::ExampleActionsECS& ex) -> std::string
  {
    if (ex.isProcessingAction())
    {
      return std::string();
    }

    // We unfreeze the perception the first time we see that processing has finished
    ex.getEntity().publish("FreezePerception", false);

    if (ex.lastActionResult[0].success())
    {
      RLOG_CPP(0, "SUCCESS");
      return "SUCCESS";
    }

    std::string fbmsgAsString = "No solution found:\n";
    std::string fbLine, fbLinePrev;
    for (size_t i = 0; i<ex.lastActionResult.size(); ++i)
    {
      const aff::ActionResult& fb = ex.lastActionResult[i];
      fbLine = fb.reason + " Suggestion: " + fb.suggestion + "\n";

      if (fbLine!=fbLinePrev)
      {
        fbmsgAsString += "  Issue " + std::to_string(i) + ": " + fbLine;
      }
      fbLinePrev = fbLine;
    }

    RLOG_CPP(0, fbmsgAsString);

    return fbmsgAsString;
  })

  .def("plan", [](aff::ExampleActionsECS& ex, std::string sequenceCommand) -> bool
  {
    PollBlockerComponent blocker(&ex);
    ex.getEntity().publish("PlanDFSEE", sequenceCommand);
    blocker.wait();
    bool success = ex.lastActionResult[0].success();
    RLOG(0, "   success=%s   result=%s", success ? "true" : "false", ex.lastActionResult[0].error.c_str());

    return success;
  })

  //////////////////////////////////////////////////////////////////////////////
  // Adds a component to listen to the Respeaker ROS nose, and to acquire the
  // sound directions, ASR etc.
  //////////////////////////////////////////////////////////////////////////////
  .def("addRespeaker", [](aff::ExampleActionsECS& ex,
                          bool listenWitHandRaisedOnly,
                          bool gazeAtSpeaker,
                          bool speakOut) -> bool
  {
    if (!ex.getScene())
    {
      RLOG(0, "Initialize ExampleActionsECS before adding Respeaker - skipping");
      return false;
    }

    aff::ComponentBase* respeaker = createComponent(ex.getEntity(), ex.getGraph(), ex.getScene(), "-respeaker");
    if (respeaker)
    {
      respeaker->setParameter("PublishDialogueWithRaisedHandOnly", listenWitHandRaisedOnly);
      ex.addComponent(respeaker);
      return true;
    }

    RLOG(1, "Can't instantiate respeaker");
    return false;
  })

  //////////////////////////////////////////////////////////////////////////////
  // Sets or clears the talk flag. This only has an effect if the Respeaker
  // component has been added, and the ASR module is running.
  //////////////////////////////////////////////////////////////////////////////
  .def("enableASR", [](aff::ExampleActionsECS& ex, bool enable)
  {
    ex.getEntity().publish("EnableASR", enable);
    RLOG(0, "%s ASR", enable ? "Enabling" : "Disabling");
  })

  //////////////////////////////////////////////////////////////////////////////
  // Adds a component to listen to the landmarks publishers through ROS, which
  // is for instance the Azure Kinect, and later also the Mediapipe components
  //////////////////////////////////////////////////////////////////////////////
  .def("addLandmarkROS", [](aff::ExampleActionsECS& ex) -> bool
  {
    RCHECK_MSG(ex.getScene(), "Initialize ExampleActionsECS before adding PTU");
    aff::ComponentBase* c = createComponent(ex.getEntity(), ex.getGraph(), ex.getScene(), "-landmarks_ros");
    if (c)
    {
      ex.addComponent(c);
      aff::LandmarkBase* lmc = dynamic_cast<aff::LandmarkBase*>(c);
      RCHECK(lmc);
      lmc->enableDebugGraphics(ex.getViewer());
      return true;
    }

    return true;
  })

  //////////////////////////////////////////////////////////////////////////////
  // Adds a component to listen to the landmarks publishers through ROS, which
  // is for instance the Azure Kinect, and later also the Mediapipe components
  //////////////////////////////////////////////////////////////////////////////
  .def("addLandmarkZmq", [](aff::ExampleActionsECS& ex) -> bool
  {
    RCHECK_MSG(ex.getScene(), "Initialize ExampleActionsECS before adding PTU");

    RLOG(0, "Adding trackers");
    std::string connection="tcp://localhost:5555";
    double r_agent = DBL_MAX;
    // argP.getArgument("-r_agent", &r_agent, "Radius around skeleton default position to start tracking (default: inf)");

    auto lmc = new aff::LandmarkZmqComponent(&ex.getEntity(), connection);
    ex.addComponent(lmc);   // Takes care of deletion
    lmc->setScenePtr(ex.getGraph(), ex.getScene());

    const RcsBody* cam = RcsGraph_getBodyByName(ex.getGraph(), "camera");
    RCHECK(cam);
    lmc->addArucoTracker(cam->name, "aruco_base");

    // Add skeleton tracker and ALL agents in the scene
    int nSkeletons = lmc->addSkeletonTrackerForAgents(r_agent);
    lmc->enableDebugGraphics(ex.getViewer());
    RLOG(0, "Added skeleton tracker with %d agents", nSkeletons);

    // Initialize all tracker camera transforms from the xml file
    lmc->setCameraTransform(&cam->A_BI);
    RLOG(0, "Done adding trackers");

    return true;
  })

  //////////////////////////////////////////////////////////////////////////////
  // Adds a component to connect to the PTU action server ROS node, and to being
  // able to send pan / tilt commands to the PTU
  //////////////////////////////////////////////////////////////////////////////
  .def("addPTU", [](aff::ExampleActionsECS& ex) -> bool
  {
    RCHECK_MSG(ex.getScene(), "Initialize ExampleActionsECS before adding PTU");
    aff::ComponentBase* c = createComponent(ex.getEntity(), ex.getGraph(), ex.getScene(), "-ptu");
    if (c)
    {
      ex.addHardwareComponent(c);
      return true;
    }

    return false;
  })

  //////////////////////////////////////////////////////////////////////////////
  // Sets the path for finding the piper TTS executables, libraries and voices
  //////////////////////////////////////////////////////////////////////////////
  .def_static("setPiperPath", &aff::TTSComponent::setPiperPath)

  //////////////////////////////////////////////////////////////////////////////
  // Adds a component to connect to enable the text-to-speech functionality.
  // Currently, 2 modes are supported: the Nuance TTS which requires the
  // corresponding ROS node to run, and a native Unix espeak TTS.
  //////////////////////////////////////////////////////////////////////////////
  .def("addTTS", [](aff::ExampleActionsECS& ex, std::string type) -> bool
  {
    if (type == "nuance")
    {
      auto c = createComponent(ex.getEntity(), ex.getGraph(),
                               ex.getScene(), "-nuance_tts");
      ex.addComponent(c);
      return c ? true : false;
    }
    else if (type == "native")
    {
      auto c = createComponent(ex.getEntity(), ex.getGraph(),
                               ex.getScene(), "-tts");
      ex.addComponent(c);
      return c ? true : false;
    }
    else if (type == "piper" || type=="piper_kathleen")
    {
      auto c = createComponent(ex.getEntity(), ex.getGraph(),
                               ex.getScene(), "-piper_tts_kathleen");
      ex.addComponent(c);
      return c ? true : false;
    }
    else if (type == "piper_alan")
    {
      auto c = createComponent(ex.getEntity(), ex.getGraph(),
                               ex.getScene(), "-piper_tts_alan");
      ex.addComponent(c);
      return c ? true : false;
    }
    else if (type == "piper_joe")
    {
      auto c = createComponent(ex.getEntity(), ex.getGraph(),
                               ex.getScene(), "-piper_tts_joe");
      ex.addComponent(c);
      return c ? true : false;
    }

    return false;
  })

  //////////////////////////////////////////////////////////////////////////////
  // Adds a component to connect to a websocket client. The component receives
  // action commands, and sends back the state.
  //////////////////////////////////////////////////////////////////////////////
  .def("addWebsocket", [](aff::ExampleActionsECS& ex) -> bool
  {
    auto c = createComponent(ex.getEntity(), ex.getGraph(),
                             ex.getScene(), "-websocket");
    ex.addComponent(c);
    return c ? true : false;
  })

  //////////////////////////////////////////////////////////////////////////////
  // Adds a component to connect to the left Jaco7 Gen2 arm
  //////////////////////////////////////////////////////////////////////////////
  .def("addJacoLeft", [](aff::ExampleActionsECS& ex) -> bool
  {
    auto c = aff::createComponent(ex.getEntity(), ex.getGraph(),
                                  ex.getScene(), "-jacoShm7l");
    ex.addHardwareComponent(c);
    return c ? true : false;
  })

  //////////////////////////////////////////////////////////////////////////////
  // Adds a component to connect to the right Jaco7 Gen2 arm
  //////////////////////////////////////////////////////////////////////////////
  .def("addJacoRight", [](aff::ExampleActionsECS& ex) -> bool
  {
    auto c = aff::createComponent(ex.getEntity(), ex.getGraph(),
                                  ex.getScene(), "-jacoShm7r");
    ex.addHardwareComponent(c);
    return c ? true : false;
  })
  .def("getCompletedActionStack", &aff::ExampleActionsECS::getCompletedActionStack)
  .def("isFinalPoseRunning", &aff::ExampleActionsECS::isFinalPoseRunning)
  .def("isProcessingAction", &aff::ExampleActionsECS::isProcessingAction)
  .def("step", &aff::ExampleActionsECS::step)
  .def("stop", &aff::ExampleActionsECS::stop)
  .def("isRunning", &aff::ExampleActionsECS::isRunning)

  //////////////////////////////////////////////////////////////////////////////
  // Scales the durations of actions (global scope)
  //////////////////////////////////////////////////////////////////////////////
  .def("setDurationScaling", [](aff::ExampleActionsECS& ex, double value)
  {
    aff::PredictionTree::setTurboDurationScaler(value);
  })
  .def("setDefaultDurationScaling", [](aff::ExampleActionsECS& ex)
  {
    aff::PredictionTree::setTurboDurationScaler(aff::PredictionTree::getDefaultTurboDurationScaler());
  })
  .def("getDurationScaling", [](aff::ExampleActionsECS& ex) -> double
  {
    return aff::PredictionTree::getTurboDurationScaler();
  })

  //////////////////////////////////////////////////////////////////////////////
  // Gaze model methods: 0: Neck only, 1: pupils only.
  //////////////////////////////////////////////////////////////////////////////
  .def("setPupilSpeedWeight", [](aff::ExampleActionsECS& ex, double value)
  {
    ex.getEntity().publish("SetPupilSpeedWeight", value);
  })
  //////////////////////////////////////////////////////////////////////////////
  // Pupil point in screen coordinates: z points outwards, x points left, y
  // points down. Origin is screen center. TODO: Make threadsafe
  //////////////////////////////////////////////////////////////////////////////
  .def("getPupilCoordinates", [](aff::ExampleActionsECS& ex) -> std::pair<std::vector<double>, std::vector<double>>
  {
    double pr[3], pl[3];
    std::vector<double> xy_right, xy_left;

    bool success = aff::ActionEyeGaze::computePupilCoordinates(ex.getGraph(), pr, pl);

    if (success)
    {
      xy_right= std::vector<double>(pr, pr+3);
      xy_left = std::vector<double>(pl, pl+3);
    }

    return std::make_pair(xy_right, xy_left);
  })

  //////////////////////////////////////////////////////////////////////////////
  // Expose several internal variables to the python layer
  //////////////////////////////////////////////////////////////////////////////
  .def_readwrite("unittest", &aff::ExampleActionsECS::unittest)
  .def_readwrite("noTextGui", &aff::ExampleActionsECS::noTextGui)
  .def_readwrite("speedUp", &aff::ExampleActionsECS::speedUp)
  .def_readwrite("xmlFileName", &aff::ExampleActionsECS::xmlFileName)
  .def_readwrite("configDirectory", &aff::ExampleActionsECS::configDirectory)
  .def_readwrite("noLimits", &aff::ExampleActionsECS::noLimits)
  .def_readwrite("noCollCheck", &aff::ExampleActionsECS::noCollCheck)   // Set before init()
  .def_readwrite("noTrajCheck", &aff::ExampleActionsECS::noTrajCheck)
  .def_readwrite("hasBeenStopped", &aff::ExampleActionsECS::hasBeenStopped)
  .def_readwrite("verbose", &aff::ExampleActionsECS::verbose)
  .def_readwrite("noViewer", &aff::ExampleActionsECS::noViewer)
  .def_readwrite("virtualCameraWidth", &aff::ExampleActionsECS::virtualCameraWidth)
  .def_readwrite("virtualCameraHeight", &aff::ExampleActionsECS::virtualCameraHeight)
  .def_readwrite("virtualCameraEnabled", &aff::ExampleActionsECS::virtualCameraEnabled)
  .def_readwrite("virtualCameraWindowEnabled", &aff::ExampleActionsECS::virtualCameraWindowEnabled)
  .def_readwrite("turbo", &aff::ExampleActionsECS::turbo)
  .def_readwrite("maxNumThreads", &aff::ExampleActionsECS::maxNumThreads)
  ;








  //////////////////////////////////////////////////////////////////////////////
  // LandmarkBase perception class wrapper
  //////////////////////////////////////////////////////////////////////////////
  py::class_<aff::LandmarkBase>(m, "LandmarkBase")
  .def(py::init<>())
  .def(py::init<>([](py::object obj)
  {
    aff::ExampleActionsECS* sim = obj.cast<aff::ExampleActionsECS*>();
    RLOG_CPP(1, sim->help());
    auto lm = std::unique_ptr<aff::LandmarkBase>(new aff::LandmarkBase());
    lm->setScenePtr(sim->getGraph(), sim->getScene());

    sim->getEntity().subscribe("PostUpdateGraph", &aff::LandmarkBase::onPostUpdateGraph, lm.get());
    sim->getEntity().subscribe("FreezePerception", &aff::LandmarkBase::onFreezePerception, lm.get());

    return std::move(lm);
  }))
  .def("addArucoTracker", &aff::LandmarkBase::addArucoTracker)
  .def("addSkeletonTrackerForAgents_org", &aff::LandmarkBase::addSkeletonTrackerForAgents)
  .def("addSkeletonTrackerForAgents", [](aff::LandmarkBase& lm, py::object sim_, double r) -> int
  {
    aff::ExampleActionsECS* sim = sim_.cast<aff::ExampleActionsECS*>();
    if (!lm.getScene())
    {
      RLOG(0, "Can't add skeleton tracker for agents - scene has not been set");
      return 0;
    }

    int numHumanAgents = 0;
    for (const auto& agent : lm.getScene()->agents)
    {
      if (dynamic_cast<aff::HumanAgent*>(agent))
      {
        numHumanAgents++;
      }
    }

    if (numHumanAgents == 0)
    {
      RLOG(0, "Can't add skeleton tracker for agents - no human agent found");
      return 0;
    }

    auto tracker = new aff::AzureSkeletonTracker(numHumanAgents);
    tracker->setScene(lm.getScene());
    lm.addTracker(std::unique_ptr<aff::AzureSkeletonTracker>(tracker));
    tracker->addAgents();
    tracker->setSkeletonDefaultPositionRadius(r);
    tracker->registerAgentAppearDisappearCallback([sim](const std::string& agentName, bool appear)
    {
      std::string appearStr = appear ? " appeared" : " disappered";
      RLOG_CPP(0, "Agent " << agentName << appearStr);
      sim->getEntity().publish("AgentChanged", agentName, appear);
    });

    return numHumanAgents;
  })
  .def("setJsonInput", &aff::LandmarkBase::setJsonInput)
  .def("getTrackerState", &aff::LandmarkBase::getTrackerState)
  .def("startCalibration", &aff::LandmarkBase::startCalibration)
  .def("isCalibrating", &aff::LandmarkBase::isCalibrating)
  .def("setSyncInputWithWallclock", &aff::LandmarkBase::setSyncInputWithWallclock)
  .def("getSyncInputWithWallclock", &aff::LandmarkBase::getSyncInputWithWallclock)
  .def("enableDebugGraphics", [](aff::LandmarkBase& lm, py::object obj)
  {
    aff::ExampleActionsECS* sim = obj.cast<aff::ExampleActionsECS*>();
    lm.enableDebugGraphics(sim->getViewer());
  })
  .def("setCameraTransform", [](aff::LandmarkBase& lm, std::string cameraName)
  {
    const RcsBody* cam = RcsGraph_getBodyByName(lm.getGraph(), cameraName.c_str());
    lm.setCameraTransform(&cam->A_BI);
  })
  .def("getAffordanceFrame", [](aff::LandmarkBase& lm, std::string bodyName, aff::Affordance::Type affordanceType) -> nlohmann::json
  {
    std::vector<std::string> frames;
    nlohmann::json data;

    const aff::AffordanceEntity* entity = lm.getScene()->getAffordanceEntity(bodyName);

    if (entity)
    {
      for (aff::Affordance* affordance : entity->affordances)
      {
        if (affordance->classType == affordanceType)
        {
          frames.push_back(affordance->frame);
        }
      }
    }
    else
    {
      NLOG(0, "Entity `%s` found in scene!", bodyName.c_str());
    }

    for (auto f : frames)
    {
      const RcsBody* b = RcsGraph_getBodyByName(lm.getGraph(), f.c_str());

      data[f] = {};
      data[f]["position"] = b->A_BI.org;

      double ea[3];
      Mat3d_toEulerAngles(ea, (double(*)[3])b->A_BI.rot);
      data[f]["euler_xyzr"] = ea;
    }

    return data;
  })
  ;







  //////////////////////////////////////////////////////////////////////////////
  // Rcs function wrappers
  //////////////////////////////////////////////////////////////////////////////

  // Sets the rcs log level
  m.def("setLogLevel", [](int level)
  {
    RcsLogLevel = level;
  });

  // Adds a directory to the resource path
  m.def("addResourcePath", [](const char* path)
  {
    return Rcs_addResourcePath(path);
  });

  // Prints the resource path to the console
  m.def("printResourcePath", []()
  {
    Rcs_printResourcePath();
  });

  m.def("getWallclockTime", []()
  {
    // Get the current time point
    auto currentTime = std::chrono::system_clock::now();

    // Convert the time point to a duration since the epoch
    std::chrono::duration<double> durationSinceEpoch = currentTime.time_since_epoch();

    // Convert the duration to seconds as a floating-point number
    double seconds = durationSinceEpoch.count();

    return seconds;
  });

}
