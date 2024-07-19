/*******************************************************************************

  Copyright (c) Honda Research Institute Europe GmbH.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.

  3. Neither the name of the copyright holder nor the names of its
     contributors may be used to endorse or promote products derived from
     this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER "AS IS" AND ANY EXPRESS OR
  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
  IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
  OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
  EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*******************************************************************************/

#ifndef AFF_AGENT_H
#define AFF_AGENT_H

#include "Manipulator.h"


namespace aff
{

class ActionScene;

class Agent : public SceneEntity
{
public:

  std::vector<std::string> manipulators;

  Agent(const xmlNodePtr node, const std::string& groupSuffix, const ActionScene* scene);
  virtual ~Agent() = default;

  static Agent* createAgent(const xmlNodePtr node, const std::string& groupSuffix, ActionScene* scene);
  virtual void print() const;
  virtual Agent* clone() const;
  virtual std::string isLookingAt() const;
  virtual bool canReachTo(const ActionScene* scene,
                          const RcsGraph* graph,
                          const double position[3]) const;
  std::vector<const AffordanceEntity*> getObjectsInReach(const ActionScene* scene,
                                                         const RcsGraph* graph) const;
  virtual bool isVisible() const;
  virtual bool check(const ActionScene* scene,
                     const RcsGraph* graph) const;
  virtual std::vector<const Manipulator*> getManipulatorsOfType(const ActionScene* scene,
                                                                const std::string& type) const;
  static Agent* getAgentOwningManipulator(const ActionScene* scene,
                                          const std::string& manipulatorName);
};

class RobotAgent : public Agent
{
public:
  RobotAgent(const xmlNodePtr node, const std::string& groupSuffix, const ActionScene* scene);
  static int getPanTilt(const RcsGraph* graph, const std::string& gazeTarget,
                        double panTilt[2], size_t maxIter, double eps,
                        double err[2]);
  Agent* clone() const;
  bool canReachTo(const ActionScene* scene,
                  const RcsGraph* graph,
                  const double position[3]) const;
  bool check(const ActionScene* scene, const RcsGraph* graph) const;
};

class HumanAgent : public Agent
{
public:
  HumanAgent(const xmlNodePtr node,
             const std::string& groupSuffix,
             const ActionScene* scene);

  Agent* clone() const;
  void setVisibility(const bool newVisibilty);
  bool hasHead(const RcsGraph* graph) const;
  bool getHeadPositionInWorld(double pos[3], const RcsGraph* graph) const;
  bool getGazeDirectionInWorld(double dir[3], const RcsGraph* graph) const;
  double getDefaultPosition(size_t index) const;
  std::vector<double> getDefaultPosition() const;
  void setDefaultPosition(const double pos[3]);

  // Remembers old one in gazeTargetPrev
  void setGazeTarget(const std::string& newGazeTarget);
  bool gazeTargetChanged() const;
  std::string getGazeTarget() const;

  void setMarkers(const std::vector<HTr>& markers);
  bool hasMarkers() const;
  HTr getMarker(size_t index) const;

  std::string isLookingAt() const;
  bool isVisible() const;
  void setLastTimeSeen(double time);
  bool canReachTo(const ActionScene* scene,
                  const RcsGraph* graph,
                  const double position[3]) const;

  // All values in world coordinates. vertices must be NULL or of size 8*3 (shape doesn't matter).
  bool computeAABB(double xyzMin[3], double xyzMax[3], MatNd* vertices) const;
  bool check(const ActionScene* scene, const RcsGraph* graph) const;

private:
  double lastTimeSeen;
  bool visible;
  std::string tracker;
  std::vector<HTr> markers;   // Vector of tracked body links
  double defaultRadius;
  std::vector<double> defaultPos;

  std::string gazeTarget;
  std::string gazeTargetPrev;
  std::string headBdyName;
  std::string leftHandBdyName;
  std::string rightHandBdyName;

};

} // namespace aff

#endif // AFF_AGENT_H
