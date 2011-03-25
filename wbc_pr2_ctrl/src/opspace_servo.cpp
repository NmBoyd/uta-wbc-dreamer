/*
 * Copyright (c) 2011 Stanford University and Willow Garage, Inc.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>
 */

/**
   \file opspace_servo.cpp Operational-space controller, uses wbc_opspace package.
   \author Roland Philippsen
*/

#include <ros/ros.h>
#include <wbc_pr2_ctrl/mq_robot_api.h>
#include <wbc_urdf/Model.hpp>
#include <jspace/Model.hpp>
#include <tao/dynamics/taoNode.h>
#include <opspace/Behavior.hpp>
#include <opspace/Factory.hpp>
#include <opspace/ControllerNG.hpp>
#include <wbc_opspace/util.h>
#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <map>
#include <err.h>

using namespace wbc_pr2_ctrl;
using namespace wbc_msgs;
using namespace opspace;
using namespace boost;
using namespace std;


static char const * opspace_fallback_str = 
  "- tasks:\n"
  "  - type: opspace::PositionTask\n"
  "    name: eepos_instance\n"
  "    end_effector_id: 6\n"
  "    dt_seconds: 0.002\n"
  "    kp: [ 100.0 ]\n"
  "    kd: [  20.0 ]\n"
  "    maxvel: [ 0.5 ]\n"
  "    maxacc: [ 1.5 ]\n"
  "  - type: opspace::PostureTask\n"
  "    name: posture_instance\n"
  "    dt_seconds: 0.002\n"
  "    kp: [ 100.0 ]\n"
  "    kd: [  20.0 ]\n"
  "    maxvel: [ 3.1416 ]\n"
  "    maxacc: [ 6.2832 ]\n"
  "- behaviors:\n"
  "  - type: opspace::TPBehavior\n"
  "    name: tpb\n"
  "    default:\n"
  "      eepos: eepos_instance\n"
  "      posture: posture_instance\n";


static jspace::State jspace_state;
static scoped_ptr<jspace::ros::Model> jspace_ros_model;
static scoped_ptr<jspace::Model> jspace_model;
static size_t ndof;
static shared_ptr<opspace::Factory> factory;
static shared_ptr<ControllerNG> controller;
static wbc_opspace::ParamCallbacks param_callbacks;


int main(int argc, char*argv[])
{
  //////////////////////////////////////////////////
  // init
  
  ros::init(argc, argv, "opspace_servo", ros::init_options::NoSigintHandler);
  ros::NodeHandle nn("~");
  factory.reset(new opspace::Factory());
  
  //////////////////////////////////////////////////
  // parse options
  
  std::string opspace_filename("");
  bool verbose(false);
  
  for (int ii(1); ii < argc; ++ii) {
    if ((strlen(argv[ii]) < 2) || ('-' != argv[ii][0])) {
      errx(EXIT_FAILURE, "problem with option `%s'", argv[ii]);
    }
    else
      switch (argv[ii][1]) {
	
      case 'b':
 	++ii;
 	if (ii >= argc) {
	  errx(EXIT_FAILURE, "-b requires parameter");
 	}
	opspace_filename = argv[ii];
 	break;
	
      case 'v':
	verbose = true;
 	break;
	
      default:
	errx(EXIT_FAILURE, "invalid option `%s'", argv[ii]);
      }
  }
  
  static bool const unlink_mqueue(true);
  MQRobotAPI robot(unlink_mqueue);
  
  ROS_INFO ("creating model via URDF conversion from ROS parameter server");
  try {
    
    jspace_ros_model.reset(new jspace::ros::Model("/wbc_pr2_ctrl/"));
    static const size_t n_tao_trees(2);
    jspace_ros_model->initFromParam(nn, "/robot_description", n_tao_trees);
    jspace_model.reset(new jspace::Model());
    if (0 != jspace_model->init(jspace_ros_model->tao_trees_[0],
				jspace_ros_model->tao_trees_[1],
				&cerr)) {
      throw std::runtime_error("jspace_model->init() failed");
    }
    
    ROS_INFO ("gravity compensation hack...");
    std::vector<std::string>::const_iterator
      gclink(jspace_ros_model->gravity_compensated_links_.begin());
    for (/**/; gclink != jspace_ros_model->gravity_compensated_links_.end(); ++gclink) {
      taoDNode const * node(jspace_model->getNodeByName(*gclink));
      if ( ! node) {
	throw std::runtime_error("gravity-compensated link " + *gclink
				 + " is not part of the jspace::Model");
      }
      int const id(node->getID());
      jspace_model->disableGravityCompensation(id, true);
      ROS_INFO ("disabled gravity compensation for link %s (ID %d)", gclink->c_str(), id);
    }
    
    ndof = jspace_model->getNDOF();
    jspace_state.init(ndof, ndof, 0);
    
    ROS_INFO ("initializing MQRobotAPI with %zu degrees of freedom", ndof);
    robot.init(true, "wbc_pr2_ctrl_s2r", "wbc_pr2_ctrl_r2s", ndof, ndof, ndof, ndof);
  }
  
  catch (std::exception const & ee) {
    ROS_ERROR ("EXCEPTION %s", ee.what());
    exit(EXIT_FAILURE);
  }

  ROS_INFO ("parsing opspace tasks and behaviors");
  shared_ptr<Behavior> behavior; // for now, just use the first one we encounter
  try {
    
    Status st;
    if (opspace_filename.empty()) {
      ROS_WARN ("no opspace_filename -- using fallback task/posture behavior");
      st = factory->parseString(opspace_fallback_str);
    }
    else {
      ROS_INFO ("opspace filename is %s", opspace_filename.c_str());
      st = factory->parseFile(opspace_filename);
    }
    if ( ! st) {
      throw runtime_error("failed to parse opspace tasks and behaviors: " + st.errstr);
    }
    factory->dump(cout, "*** dump of opspace task/behavior factory", "* ");
    if (factory->getTaskTable().empty()) {
      throw runtime_error("empty opspace task table");
    }
    if (factory->getBehaviorTable().empty()) {
      throw runtime_error("empty opspace behavior table");
    }
    behavior = factory->getBehaviorTable()[0]; // XXXX to do: allow selection at runtime
  }
  catch (std::exception const & ee) {
    ROS_ERROR ("EXCEPTION %s", ee.what());
    exit(EXIT_FAILURE);
  }
  
  ROS_INFO ("initializing state, model, tasks, behaviors, ...");
  if ( ! robot.readState(jspace_state)) {
    ROS_ERROR ("robot.readState() failed");
    exit(EXIT_FAILURE);
  }
  jspace_model->update(jspace_state);
  controller.reset(new ControllerNG("opspace_servo"));
  jspace::Status status;
  status = controller->init(*jspace_model);
  if ( ! status) {
    ROS_ERROR ("controller->init() failed: %s", status.errstr.c_str());
    exit(EXIT_FAILURE);
  }
  status = behavior->init(*jspace_model);
  if ( ! status) {
    ROS_ERROR ("behavior->init() failed: %s", status.errstr.c_str());
    exit(EXIT_FAILURE);
  }
  
  ROS_INFO ("starting services");
  try {
    param_callbacks.init(nn, factory, controller, 1, 100);
  }
  catch (std::exception const & ee) {
    ROS_ERROR ("EXCEPTION %s", ee.what());
    exit(EXIT_FAILURE);
  }
  
  ROS_INFO ("entering control loop");
  jspace::Vector tau(ndof);
  ros::Time t0(ros::Time::now());
  ros::Duration dbg_dt(0.2);
  while (ros::ok()) {
    
    // Compute torque command.
    status = controller->computeCommand(*jspace_model, *behavior, tau);
    if ( ! status) {
      ROS_ERROR ("controller->computeCommand() failed: %s", status.errstr.c_str());
      ros::shutdown();
      break;
    }
    
    // Send it to the robot.
    status = robot.writeCommand(tau);
    if ( ! status) {
      ROS_ERROR ("robot.writeCommand() failed: %s", status.errstr.c_str());
      ros::shutdown();
      break;
    }
    
    if (verbose) {
      ros::Time t1(ros::Time::now());
      if (t1 - t0 > dbg_dt) {
	t0 = t1;
	behavior->dbg(cout, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~", "");
	controller->dbg(cout, "--------------------------------------------------", "");
      }
    }
    
    // Wait for "tick" (haha, this is not RT anyway...)
    ros::spinOnce();
    
    // Read the robot state and update the model
    if ( ! robot.readState(jspace_state)) {
      ROS_ERROR ("robot.readState() failed");
      ros::shutdown();
      break;
    }
    jspace_model->update(jspace_state);
    
  }
  
  for (size_t ii(10); ros::ok() && (ii > 0); --ii) {
    if (10 == ii) {
      cerr << "waiting 10 seconds for node shutdown...";
    }
    cerr << ii << "..." << flush;
    usleep(1000000);
  }
  if (ros::ok()) {
    cerr << "giving up\n";
  }
  else {
    cerr << "done\n";
  }
}
