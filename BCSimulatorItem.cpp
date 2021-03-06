/* 
 This file is a part of BetterContactPlugin.
 
 Author: Shin'ichiro Nakaoka
 Author: Ryo Kikuuwe
 
 Copyright (c) 2007-2015 Shin'ichiro Nakaoka
 Copyright (c) 2014-2015 Ryo Kikuuwe
 Copyright (c) 2007-2015 National Institute of Advanced Industrial
                         Science and Technology (AIST)
 Copyright (c) 2014-2015 Kyushu University

 BetterContactPlugin is a plugin for better simulation of frictional contacts.
 
 BetterContactPlugin is a free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 BetterContactPlugin is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with BetterContactPlugin; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 
 Contact: Ryo Kikuuwe, kikuuwe@ieee.org
*/
/*************************************************

ORIGINAL FILE: src/BodyPlugin/AISTSimulatorItem.cpp

**************************************************/


#include "BCSimulatorItem.h"
#include <cnoid/BodyItem>
#include <cnoid/BodyMotionItem>
#include <cnoid/ControllerItem>
#include <cnoid/ItemManager>
#include <cnoid/Archive>
#include <cnoid/EigenArchive>
#include <cnoid/DyWorld>
#include <cnoid/DyBody>
#include <cnoid/ForwardDynamicsCBM>
#include "BCConstraintForceSolver.h"
#include <cnoid/LeggedBodyHelper>
#include <cnoid/FloatingNumberString>
#include <cnoid/EigenUtil>
#include <cnoid/MessageView>
#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>
#include <iostream>
#include <iomanip>
#include "gettext.h"

using namespace std;
using namespace cnoid;
using boost::format;

// for Windows
#undef min
#undef max

namespace {

const bool TRACE_FUNCTIONS = false;
const bool ENABLE_DEBUG_OUTPUT = false;
const double DEFAULT_GRAVITY_ACCELERATION = 9.80665;


class HighGainControllerItem : public ControllerItem
{
    BodyPtr body;
    MultiValueSeqPtr qseqRef;
    int currentFrame;
    int lastFrame;
    int numJoints;

public:
    HighGainControllerItem(BodyItem* bodyItem, BodyMotionItem* bodyMotionItem) {
        qseqRef = bodyMotionItem->jointPosSeq();
        setName(str(fmt(_("HighGain Controller with %1%")) % bodyMotionItem->name()));
    }

    virtual bool start(Target* target) {
        body = target->body();
        currentFrame = 0;
        lastFrame = std::max(0, qseqRef->numFrames() - 1);
        numJoints = std::min(body->numJoints(), qseqRef->numParts());
        if(qseqRef->numFrames() == 0){
            putMessage(_("Reference motion is empty()."));
            return false;
        }
        if(fabs(qseqRef->frameRate() - (1.0 / target->worldTimeStep())) > 1.0e-6){
            putMessage(_("The frame rate of the reference motion is different from the world frame rate."));
            return false;
        }
        control();
        return true;
    }

    virtual double timeStep() const {
        return qseqRef->getTimeStep();
    }
        
    virtual void input() { }

    virtual bool control() {

        if(++currentFrame > lastFrame){
            currentFrame = lastFrame;
            return false;
        }
        return true;
    }
        
    virtual void output() {

        int prevFrame = std::max(currentFrame - 1, 0);
        int nextFrame = std::min(currentFrame + 1, lastFrame);
            
        MultiValueSeq::Frame q0 = qseqRef->frame(prevFrame);
        MultiValueSeq::Frame q1 = qseqRef->frame(currentFrame);
        MultiValueSeq::Frame q2 = qseqRef->frame(nextFrame);

        double dt = qseqRef->getTimeStep();
        double dt2 = dt * dt;

        for(int i=0; i < numJoints; ++i){
            Link* joint = body->joint(i);
            joint->q() = q1[i];
            joint->dq() = (q2[i] - q1[i]) / dt;
            joint->ddq() = (q2[i] - 2.0 * q1[i] + q0[i]) / dt2;
        }
    }
        
    virtual void stop() { }
};


class BCSimBody : public SimulationBody
{
public:
    BCSimBody(DyBody* body) : SimulationBody(body) { }
};
    

class KinematicWalkBody : public BCSimBody
{
public:
    KinematicWalkBody(DyBody* body, LeggedBodyHelper* legged)
        : BCSimBody(body),
          legged(legged) {
        supportFootIndex = 0;
        for(int i=1; i < legged->numFeet(); ++i){
            if(legged->footLink(i)->p().z() < legged->footLink(supportFootIndex)->p().z()){
                supportFootIndex = i;
            }
        }
        traverse.find(legged->footLink(supportFootIndex), true, true);
    }
    LeggedBodyHelper* legged;
    int supportFootIndex;
    LinkTraverse traverse;
};

}

namespace cnoid {
  
class BCSimulatorItemImpl
{
public:
    BCSimulatorItem* self;

    World<BCConstraintForceSolver> world;
        
    Selection dynamicsMode;
    Selection integrationMode;
/*BC*/ Selection solverMode;
    Vector3 gravity;
    double staticFriction;
    double slipFriction;
    FloatingNumberString contactCullingDistance;
    FloatingNumberString contactCullingDepth;
    FloatingNumberString errorCriterion;
    int maxNumIterations;
    FloatingNumberString contactCorrectionDepth;
    FloatingNumberString contactCorrectionVelocityRatio;
    double epsilon;
    bool is2Dmode;
    bool isKinematicWalkingEnabled;

    typedef std::map<Body*, int> BodyIndexMap;
    BodyIndexMap bodyIndexMap;

    boost::optional<int> forcedBodyPositionFunctionId;
    boost::mutex forcedBodyPositionMutex;
    DyBody* forcedPositionBody;
    Position forcedBodyPosition;

    BCSimulatorItemImpl(BCSimulatorItem* self);
    BCSimulatorItemImpl(BCSimulatorItem* self, const BCSimulatorItemImpl& org);
    bool initializeSimulation(const std::vector<SimulationBody*>& simBodies);
    void addBody(BCSimBody* simBody);
    void clearExternalForces();
    void setForcedBodyPosition(BodyItem* bodyItem, const Position& T);
    void doSetForcedBodyPosition();
    void doPutProperties(PutPropertyFunction& putProperty);
    bool store(Archive& archive);
    bool restore(const Archive& archive);

    // for debug
    ofstream os;

    double penaltyKpCoef;     // ADDED
    double penaltyKvCoef;     // ADDED
    double penaltySizeRatio;  // ADDED
};

}


void BCSimulatorItem::initializeClass(ExtensionManager* ext)
{
    ext->itemManager().registerClass<BCSimulatorItem>(N_("BCSimulatorItem"));
    ext->itemManager().addCreationPanel<BCSimulatorItem>();
}


BCSimulatorItem::BCSimulatorItem()
{
    impl = new BCSimulatorItemImpl(this);
    setName("BCSimulator");
}
 

BCSimulatorItemImpl::BCSimulatorItemImpl(BCSimulatorItem* self)
    : self(self),
      dynamicsMode   (BCSimulatorItem::N_DYNAMICS_MODES   , CNOID_GETTEXT_DOMAIN_NAME),
      integrationMode(BCSimulatorItem::N_INTEGRATION_MODES, CNOID_GETTEXT_DOMAIN_NAME),
      solverMode     (BCSimulatorItem::N_SOLVER_MODES     , CNOID_GETTEXT_DOMAIN_NAME)
{
    dynamicsMode.setSymbol(BCSimulatorItem::FORWARD_DYNAMICS,  N_("Forward dynamics"));
    dynamicsMode.setSymbol(BCSimulatorItem::HG_DYNAMICS,       N_("High-gain dynamics"));
    dynamicsMode.setSymbol(BCSimulatorItem::KINEMATICS,        N_("Kinematics"));

    integrationMode.setSymbol(BCSimulatorItem::EULER_INTEGRATION,  N_("Euler"));
    integrationMode.setSymbol(BCSimulatorItem::RUNGE_KUTTA_INTEGRATION,  N_("Runge Kutta"));
    integrationMode.select(BCSimulatorItem::EULER_INTEGRATION);

    solverMode.setSymbol(BCSimulatorItem::SLV_GAUSS_SEIDEL ,  N_("GaussSeidel"));
    solverMode.setSymbol(BCSimulatorItem::SLV_SICONOS      ,  N_("Siconos"));
    solverMode.setSymbol(BCSimulatorItem::SLV_QMR          ,  N_("QMR(TBD)"));
    solverMode.select(BCSimulatorItem::SLV_GAUSS_SEIDEL);
    
    gravity << 0.0, 0.0, -DEFAULT_GRAVITY_ACCELERATION;

    BCConstraintForceSolver& cfs = world.constraintForceSolver;
    staticFriction = cfs.staticFriction();
    slipFriction = cfs.slipFriction();
    contactCullingDistance = cfs.contactCullingDistance();
    contactCullingDepth = cfs.contactCullingDepth();
    epsilon = cfs.coefficientOfRestitution();
    
    errorCriterion = cfs.gaussSeidelErrorCriterion();
    maxNumIterations = cfs.gaussSeidelMaxNumIterations();
    contactCorrectionDepth = cfs.contactCorrectionDepth();
    contactCorrectionVelocityRatio = cfs.contactCorrectionVelocityRatio();

    isKinematicWalkingEnabled = false;
    is2Dmode = false;
    
    penaltyKpCoef = cfs.penaltyKpCoef();         // ADDED
    penaltyKvCoef = cfs.penaltyKvCoef();         // ADDED
    penaltySizeRatio = cfs.penaltySizeRatio();   // ADDED
}


BCSimulatorItem::BCSimulatorItem(const BCSimulatorItem& org)
    : SimulatorItem(org),
      impl(new BCSimulatorItemImpl(this, *org.impl))
{

}


BCSimulatorItemImpl::BCSimulatorItemImpl(BCSimulatorItem* self, const BCSimulatorItemImpl& org)
    : self(self),
      dynamicsMode(org.dynamicsMode),
      integrationMode(org.integrationMode),
      solverMode     (org.solverMode)
{
    gravity = org.gravity;
    staticFriction = org.staticFriction;
    slipFriction = org.slipFriction;
    contactCullingDistance = org.contactCullingDistance;
    contactCullingDepth = org.contactCullingDepth;
    errorCriterion = org.errorCriterion;
    maxNumIterations = org.maxNumIterations;
    contactCorrectionDepth = org.contactCorrectionDepth;
    contactCorrectionVelocityRatio = org.contactCorrectionVelocityRatio;
    epsilon = org.epsilon;
    isKinematicWalkingEnabled = org.isKinematicWalkingEnabled;
    is2Dmode = org.is2Dmode; 
    penaltyKpCoef = org.penaltyKpCoef;       // ADDED
    penaltyKvCoef = org.penaltyKvCoef;       // ADDED
    penaltySizeRatio = org.penaltySizeRatio; // ADDED
}


BCSimulatorItem::~BCSimulatorItem()
{
    delete impl;
}


void BCSimulatorItem::setDynamicsMode(int mode)
{
    impl->dynamicsMode.select(mode);
}


void BCSimulatorItem::setIntegrationMode(int mode)
{
    impl->integrationMode.select(mode);
}


void BCSimulatorItem::setSolverMode(int mode)
{
    impl->solverMode.select(mode);
}


void BCSimulatorItem::setGravity(const Vector3& gravity)
{
    impl->gravity = gravity;
}


void BCSimulatorItem::setStaticFriction(double value)
{
    impl->staticFriction = value; 
}


void BCSimulatorItem::setSlipFriction(double value)
{
    impl->slipFriction = value;
}


void BCSimulatorItem::setContactCullingDistance(double value)    
{
    impl->contactCullingDistance = value;
}


void BCSimulatorItem::setContactCullingDepth(double value)    
{
    impl->contactCullingDepth = value;
}

    
void BCSimulatorItem::setErrorCriterion(double value)    
{
    impl->errorCriterion = value;
}

    
void BCSimulatorItem::setMaxNumIterations(int value)
{
    impl->maxNumIterations = value;   
}


void BCSimulatorItem::setContactCorrectionDepth(double value)
{
    impl->contactCorrectionDepth = value;
}


void BCSimulatorItem::setContactCorrectionVelocityRatio(double value)
{
    impl->contactCorrectionVelocityRatio = value;
}


void BCSimulatorItem::setEpsilon(double epsilon)
{
    impl->epsilon = epsilon;
}


void BCSimulatorItem::set2Dmode(bool on)
{
    impl->is2Dmode = on;
}


void BCSimulatorItem::setKinematicWalkingEnabled(bool on)
{
    impl->isKinematicWalkingEnabled = on;
}


Item* BCSimulatorItem::doDuplicate() const
{
    return new BCSimulatorItem(*this);
}


SimulationBody* BCSimulatorItem::createSimulationBody(Body* orgBody)
{
    SimulationBody* simBody = 0;
    DyBody* body = new DyBody(*orgBody);
    
    if(impl->dynamicsMode.is(KINEMATICS) && impl->isKinematicWalkingEnabled){
        LeggedBodyHelper* legged = getLeggedBodyHelper(body);
        if(legged->isValid()){
            simBody = new KinematicWalkBody(body, legged);
        }
    }
    if(!simBody){
        simBody = new BCSimBody(body);
    }

    return simBody;
}


ControllerItem* BCSimulatorItem::createBodyMotionController(BodyItem* bodyItem, BodyMotionItem* bodyMotionItem)
{
    return new HighGainControllerItem(bodyItem, bodyMotionItem);
}


bool BCSimulatorItem::initializeSimulation(const std::vector<SimulationBody*>& simBodies)
{
    return impl->initializeSimulation(simBodies);
}


bool BCSimulatorItemImpl::initializeSimulation(const std::vector<SimulationBody*>& simBodies)
{
    if(ENABLE_DEBUG_OUTPUT){
        static int ntest = 0;
        os.open((string("test-log-") + boost::lexical_cast<string>(ntest++) + ".log").c_str());
        os << setprecision(30);
    }

    if(integrationMode.is(BCSimulatorItem::EULER_INTEGRATION)){
        world.setEulerMethod();
    } else if(integrationMode.is(BCSimulatorItem::RUNGE_KUTTA_INTEGRATION)){
        world.setRungeKuttaMethod();
    } 
    world.setGravityAcceleration(gravity);
    world.enableSensors(true);
    world.setTimeStep(self->worldTimeStep());
    world.setCurrentTime(0.0);

    BCConstraintForceSolver& cfs = world.constraintForceSolver;
    if     (solverMode.is(BCSimulatorItem::SLV_GAUSS_SEIDEL ))cfs.setSolverID(0);
    else if(solverMode.is(BCSimulatorItem::SLV_SICONOS      ))cfs.setSolverID(1);
    else                                                      cfs.setSolverID(2);
    
    cfs.setGaussSeidelErrorCriterion(errorCriterion.value());
    cfs.setGaussSeidelMaxNumIterations(maxNumIterations);
    cfs.setContactDepthCorrection(
        contactCorrectionDepth.value(), contactCorrectionVelocityRatio.value());

    self->addPreDynamicsFunction(boost::bind(&BCSimulatorItemImpl::clearExternalForces, this));

    world.clearBodies();
    bodyIndexMap.clear();
    for(size_t i=0; i < simBodies.size(); ++i){
        addBody(static_cast<BCSimBody*>(simBodies[i]));
    }

    cfs.setFriction(staticFriction, slipFriction);
    cfs.setContactCullingDistance(contactCullingDistance.value());
    cfs.setContactCullingDepth(contactCullingDepth.value());
    cfs.setCoefficientOfRestitution(epsilon);
    cfs.setCollisionDetector(self->collisionDetector());
    
    if(is2Dmode){
        cfs.set2Dmode(true);
    }
    cfs.setPenaltyKpCoef(penaltyKpCoef );        // ADDED
    cfs.setPenaltyKvCoef(penaltyKvCoef );        // ADDED
    cfs.setPenaltySizeRatio(penaltySizeRatio );  // ADDED

    world.initialize();

    return true;
}


void BCSimulatorItemImpl::addBody(BCSimBody* simBody)
{
    DyBody* body = static_cast<DyBody*>(simBody->body());

    DyLink* rootLink = body->rootLink();
    rootLink->v().setZero();
    rootLink->dv().setZero();
    rootLink->w().setZero();
    rootLink->dw().setZero();
    rootLink->vo().setZero();
    rootLink->dvo().setZero();

    bool isHighGainMode = dynamicsMode.is(BCSimulatorItem::HG_DYNAMICS);
    if(dynamic_cast<HighGainControllerItem*>(simBody->controller())){
        isHighGainMode = true;
    }

    for(int i=0; i < body->numLinks(); ++i){
        Link* link = body->link(i);
        link->u() = 0.0;
        link->dq() = 0.0;
        link->ddq() = 0.0;
    }
    
    body->clearExternalForces();
    body->calcForwardKinematics(true, true);

    if(isHighGainMode){
        ForwardDynamicsCBMPtr cbm = make_shared_aligned<ForwardDynamicsCBM>(body);
        cbm->setHighGainModeForAllJoints();
        bodyIndexMap[body] = world.addBody(body, cbm);
    } else {
        bodyIndexMap[body] = world.addBody(body);
    }
}


void BCSimulatorItemImpl::clearExternalForces()
{
    world.constraintForceSolver.clearExternalForces();
}

bool BCSimulatorItem::stepSimulation(const std::vector<SimulationBody*>& activeSimBodies)
{
    if(!impl->dynamicsMode.is(KINEMATICS)){
        impl->world.calcNextState();
        return true;
    }

    // Kinematics mode
    if(!impl->isKinematicWalkingEnabled){
        for(size_t i=0; i < activeSimBodies.size(); ++i){
            activeSimBodies[i]->body()->calcForwardKinematics(true, true);
        }
    } else {
        for(size_t i=0; i < activeSimBodies.size(); ++i){
            SimulationBody* simBody = activeSimBodies[i];
            KinematicWalkBody* walkBody = dynamic_cast<KinematicWalkBody*>(simBody);
            if(!walkBody){
                simBody->body()->calcForwardKinematics(true, true);
            } else {
                walkBody->traverse.calcForwardKinematics(true, true);
                
                LeggedBodyHelper* legged = walkBody->legged;
                const int supportFootIndex = walkBody->supportFootIndex;
                int nextSupportFootIndex = supportFootIndex;
                Link* supportFoot = legged->footLink(supportFootIndex);
                Link* nextSupportFoot = supportFoot;
                const int n = legged->numFeet();
                for(int i=0; i < n; ++i){
                    if(i != supportFootIndex){
                        Link* foot = legged->footLink(i);
                        if(foot->p().z() < nextSupportFoot->p().z()){
                            nextSupportFootIndex = i;
                            nextSupportFoot = foot;
                        }
                    }
                }
                if(nextSupportFoot != supportFoot){
                    nextSupportFoot->p().z() = supportFoot->p().z();
                    walkBody->supportFootIndex = nextSupportFootIndex;
                    supportFoot = nextSupportFoot;
                    walkBody->traverse.find(supportFoot, true, true);
                    walkBody->traverse.calcForwardKinematics(true, true);
                }
            }
        }
    }
    return true;
}


void BCSimulatorItem::finalizeSimulation()
{
    if(ENABLE_DEBUG_OUTPUT){
        impl->os.close();
    }
}

CollisionLinkPairListPtr BCSimulatorItem::getCollisions()
{
    return impl->world.constraintForceSolver.getCollisions();
}

void BCSimulatorItem::setForcedBodyPosition(BodyItem* bodyItem, const Position& T)
{
    impl->setForcedBodyPosition(bodyItem, T);
}


void BCSimulatorItemImpl::setForcedBodyPosition(BodyItem* bodyItem, const Position& T)
{
    if(SimulationBody* simBody = self->findSimulationBody(bodyItem)){
        {
            boost::unique_lock<boost::mutex> lock(forcedBodyPositionMutex);
            forcedPositionBody = static_cast<DyBody*>(simBody->body());
            forcedBodyPosition = T;
        }
        if(!forcedBodyPositionFunctionId){
            forcedBodyPositionFunctionId =
                self->addPostDynamicsFunction(
                    boost::bind(&BCSimulatorItemImpl::doSetForcedBodyPosition, this));
        }
    }
}


void BCSimulatorItem::clearForcedBodyPositions()
{
    if(impl->forcedBodyPositionFunctionId){
        removePostDynamicsFunction(*impl->forcedBodyPositionFunctionId);
        impl->forcedBodyPositionFunctionId = boost::none;
    }
}
    

void BCSimulatorItemImpl::doSetForcedBodyPosition()
{
    boost::unique_lock<boost::mutex> lock(forcedBodyPositionMutex);
    DyLink* rootLink = forcedPositionBody->rootLink();
    rootLink->setPosition(forcedBodyPosition);
    rootLink->v().setZero();
    rootLink->w().setZero();
    rootLink->vo().setZero();
    forcedPositionBody->calcSpatialForwardKinematics();
}

void BCSimulatorItem::doPutProperties(PutPropertyFunction& putProperty)
{
    SimulatorItem::doPutProperties(putProperty);
    impl->doPutProperties(putProperty);
}


void BCSimulatorItemImpl::doPutProperties(PutPropertyFunction& putProperty)
{
    putProperty(_("Dynamics mode"), dynamicsMode,
                boost::bind(&Selection::selectIndex, &dynamicsMode, _1));
    putProperty(_("Integration mode"), integrationMode,
                boost::bind(&Selection::selectIndex, &integrationMode, _1));
    putProperty(_("Solver mode"), solverMode,
                boost::bind(&Selection::selectIndex, &solverMode, _1));
    putProperty(_("Gravity"), str(gravity), boost::bind(toVector3, _1, boost::ref(gravity)));
    putProperty.decimals(3).min(0.0);
    putProperty(_("Static friction"), staticFriction, changeProperty(staticFriction));
    putProperty(_("Slip friction"), slipFriction, changeProperty(slipFriction));
    putProperty(_("penaltyKpCoef"), penaltyKpCoef, changeProperty(penaltyKpCoef));          // ADDED
    putProperty(_("penaltyKvCoef"), penaltyKvCoef, changeProperty(penaltyKvCoef));          // ADDED
    putProperty(_("penaltySizeRatio"), penaltySizeRatio, changeProperty(penaltySizeRatio)); // ADDED 
    putProperty(_("Contact culling distance"), contactCullingDistance,
                (boost::bind(&FloatingNumberString::setNonNegativeValue, boost::ref(contactCullingDistance), _1)));
    putProperty(_("Contact culling depth"), contactCullingDepth,
                (boost::bind(&FloatingNumberString::setNonNegativeValue, boost::ref(contactCullingDepth), _1)));
    putProperty(_("Error criterion"), errorCriterion,
                boost::bind(&FloatingNumberString::setPositiveValue, boost::ref(errorCriterion), _1));
    putProperty.min(1.0)(_("Max iterations"), maxNumIterations, changeProperty(maxNumIterations));
    putProperty(_("Contact correction depth"), contactCorrectionDepth,
                boost::bind(&FloatingNumberString::setNonNegativeValue, boost::ref(contactCorrectionDepth), _1));
    putProperty(_("Contact correction v-ratio"), contactCorrectionVelocityRatio,
                boost::bind(&FloatingNumberString::setNonNegativeValue, boost::ref(contactCorrectionVelocityRatio), _1));
    putProperty(_("Kinematic walking"), isKinematicWalkingEnabled,
                changeProperty(isKinematicWalkingEnabled));
    putProperty(_("2D mode"), is2Dmode, changeProperty(is2Dmode));
}


bool BCSimulatorItem::store(Archive& archive)
{
    SimulatorItem::store(archive);
    return impl->store(archive);
}


bool BCSimulatorItemImpl::store(Archive& archive)
{
    archive.write("dynamicsMode", dynamicsMode.selectedSymbol());
    archive.write("integrationMode", integrationMode.selectedSymbol());
    archive.write("solverMode", solverMode.selectedSymbol());
    write(archive, "gravity", gravity);
    archive.write("staticFriction", staticFriction);
    archive.write("slipFriction", slipFriction);
    archive.write("cullingThresh", contactCullingDistance);
    archive.write("contactCullingDepth", contactCullingDepth);
    archive.write("errorCriterion", errorCriterion);
    archive.write("maxNumIterations", maxNumIterations);
    archive.write("contactCorrectionDepth", contactCorrectionDepth);
    archive.write("contactCorrectionVelocityRatio", contactCorrectionVelocityRatio);
    archive.write("kinematicWalking", isKinematicWalkingEnabled);
    archive.write("2Dmode", is2Dmode);
    archive.write("penaltyKpCoef", penaltyKpCoef);       // ADDED
    archive.write("penaltyKvCoef", penaltyKvCoef);       // ADDED
    archive.write("penaltySizeRatio", penaltySizeRatio); // ADDED
    return true;
}


bool BCSimulatorItem::restore(const Archive& archive)
{
    SimulatorItem::restore(archive);
    return impl->restore(archive);
}


bool BCSimulatorItemImpl::restore(const Archive& archive)
{
    string symbol;
    if(archive.read("dynamicsMode", symbol)){
        dynamicsMode.select(symbol);
    }
    if(archive.read("integrationMode", symbol)){
        integrationMode.select(symbol);
    }
    if(archive.read("solverMode", symbol)){
        solverMode.select(symbol);
    }
    read(archive, "gravity", gravity);
    archive.read("staticFriction", staticFriction);
    archive.read("slipFriction", slipFriction);
    contactCullingDistance = archive.get("cullingThresh", contactCullingDistance.string());
    contactCullingDepth = archive.get("contactCullingDepth", contactCullingDepth.string());
    errorCriterion = archive.get("errorCriterion", errorCriterion.string());
    archive.read("maxNumIterations", maxNumIterations);
    contactCorrectionDepth = archive.get("contactCorrectionDepth", contactCorrectionDepth.string());
    contactCorrectionVelocityRatio = archive.get("contactCorrectionVelocityRatio", contactCorrectionVelocityRatio.string());
    archive.read("kinematicWalking", isKinematicWalkingEnabled);
    archive.read("2Dmode", is2Dmode);
    archive.read("penaltyKpCoef", penaltyKpCoef);         // ADDED
    archive.read("penaltyKvCoef", penaltyKvCoef);         // ADDED
    archive.read("penaltySizeRatio", penaltySizeRatio);   // ADDED
    return true;
}

#ifdef ENABLE_SIMULATION_PROFILING
void BCSimulatorItem::getProfilingNames(vector<string>& profilingNames)
{
    profilingNames.push_back("Collision detection time");
    profilingNames.push_back("Constraint force calculation time");
    profilingNames.push_back("Forward dynamics calculation time");
    profilingNames.push_back("Customizer calculation time");
}


void BCSimulatorItem::getProfilingTimes(vector<double>& profilingToimes)
{
    double collisionTime = impl->world.constraintForceSolver.getCollisionTime();
    profilingToimes.push_back(collisionTime);
    profilingToimes.push_back(impl->world.forceSolveTime - collisionTime);
    profilingToimes.push_back(impl->world.forwardDynamicsTime);
    profilingToimes.push_back(impl->world.customizerTime);
}
#endif
