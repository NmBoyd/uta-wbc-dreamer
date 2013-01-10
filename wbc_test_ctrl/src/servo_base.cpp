/*
 * Whole-Body Control for Human-Centered Robotics http://www.me.utexas.edu/~hcrl/
 *
 * Copyright (c) 2011 University of Texas at Austin. All rights reserved.
 *
 * Author: Roland Philippsen
 *
 * BSD license:
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of
 *    contributors to this software may be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR THE CONTRIBUTORS TO THIS SOFTWARE BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <wbc_test_ctrl/test_util_base.h>

#include <ros/ros.h>
#include <jspace/test/sai_util.hpp>
#include <opspace/Skill.hpp>
#include <opspace/Factory.hpp>
#include <uta_opspace/ControllerNG.hpp>
#include <uta_opspace/HelloGoodbyeSkill.hpp>
#include <uta_opspace/TaskOriPostureSkill.hpp>
#include <uta_opspace/WriteSkill.hpp>
#include <uta_opspace/JointMultiPos.hpp>
#include <uta_opspace/CartMultiPos.hpp>
#include <uta_opspace/BaseMultiPos.hpp>
#include <wbc_core/opspace_param_callbacks.hpp>
#include <boost/scoped_ptr.hpp>
#include <err.h>
#include <signal.h>
#include "comm_udp.h"

using namespace wbc_test_ctrl;
using namespace opspace;
using namespace wbc_core_opspace;
using namespace uta_opspace;
using namespace boost;
using namespace std;


static char const * opspace_fallback_str = 
  "- tasks:\n"
  "  - type: opspace::PositionTask\n"
  "    name: eepos\n"
  "    end_effector_id: 6\n"
  "    dt_seconds: 0.002\n"
  "    kp: [ 100.0 ]\n"
  "    kd: [  10.0 ]\n"
  "    maxvel: [ 0.5 ]\n"
  "    maxacc: [ 1.5 ]\n"
  "  - type: opspace::PostureTask\n"
  "    name: posture\n"
  "    dt_seconds: 0.002\n"
  "    kp: [ 100.0 ]\n"
  "    kd: [  10.0 ]\n"
  "    maxvel: [ 3.1416 ]\n"
  "    maxacc: [ 6.2832 ]\n"
  "- skills:\n"
  "  - type: opspace::TPSkill\n"
  "    name: task_posture\n"
  "    default:\n"
  "      eepos: eepos\n"
  "      posture: posture\n";


static bool verbose(false);
static scoped_ptr<jspace::Model> model;
static shared_ptr<Factory> factory;
static shared_ptr<opspace::ReflectionRegistry> registry;
static long long servo_rate;
static long long actual_servo_rate;
static shared_ptr<ParamCallbacks> param_cbs;
static shared_ptr<ControllerNG> controller;
command cmd;
jspace::Vector destination;


static void usage(int ecode, std::string msg)
{
  errx(ecode,
       "%s\n"
       "  options:\n"
       "  -h               help (this message)\n"
       "  -v               verbose mode\n"
       "  -r  <filename>   robot specification (SAI XML format)\n"
       "  -f  <frequency>  servo rate (integer number in Hz, default 500Hz)\n"
       "  -s  <filename>   skill specification (YAML file with tasks etc)",
       msg.c_str());
}


static void parse_options(int argc, char ** argv)
{
  string skill_spec("");
  string robot_spec("");
  servo_rate = 500;
  
  for (int ii(1); ii < argc; ++ii) {
    if ((strlen(argv[ii]) < 2) || ('-' != argv[ii][0])) {
      usage(EXIT_FAILURE, "problem with option `" + string(argv[ii]) + "'");
    }
    else
      switch (argv[ii][1]) {
	
      case 'h':
	usage(EXIT_SUCCESS, "servo [-h] [-v] [-s skillspec] -r robotspec");
	
      case 'v':
	verbose = true;
 	break;
	
      case 'r':
 	++ii;
 	if (ii >= argc) {
	  usage(EXIT_FAILURE, "-r requires parameter");
 	}
	robot_spec = argv[ii];
 	break;
	
      case 'f':
 	++ii;
 	if (ii >= argc) {
	  usage(EXIT_FAILURE, "-f requires parameter");
 	}
	else {
	  istringstream is(argv[ii]);
	  is >> servo_rate;
	  if ( ! is) {
	    usage(EXIT_FAILURE, "failed to read servo rate from `" + string(argv[ii]) + "'");
	  }
	  if (0 >= servo_rate) {
	    usage(EXIT_FAILURE, "servo rate has to be positive");
	  }
	}
 	break;
	
      case 's':
 	++ii;
 	if (ii >= argc) {
	  usage(EXIT_FAILURE, "-s requires parameter");
 	}
	skill_spec = argv[ii];
 	break;
	
      default:
	usage(EXIT_FAILURE, "invalid option `" + string(argv[ii]) + "'");
      }
  }
  
  try {
    if (robot_spec.empty()) {
      usage(EXIT_FAILURE, "no robot specification (see option -r)");
    }
    if (verbose) {
      warnx("reading robot spec from %s", robot_spec.c_str());
    }
    static bool const enable_coriolis_centrifugal(false);
    model.reset(jspace::test::parse_sai_xml_file(robot_spec, enable_coriolis_centrifugal));
  }
  catch (runtime_error const & ee) {
    errx(EXIT_FAILURE,
	 "exception while parsing robot specification\n"
	 "  filename: %s\n"
	 "  error: %s",
	 robot_spec.c_str(), ee.what());
  }
  
  factory.reset(new Factory());
  
  Status st;
  if (skill_spec.empty()) {
    if (verbose) {
      warnx("using fallback task/posture skill");
    }
    st = factory->parseString(opspace_fallback_str);
  }
  else {
    if (verbose) {
      warnx("reading skills from %s", skill_spec.c_str());
    }
    st = factory->parseFile(skill_spec);
  }
  if ( ! st) {
    errx(EXIT_FAILURE,
	 "failed to parse skills\n"
	 "  specification file: %s\n"
	 "  error description: %s",
	 skill_spec.c_str(), st.errstr.c_str());
  }
  if (verbose) {
    factory->dump(cout, "*** parsed tasks and skills", "* ");
  }
}


static void handle(int signum)
{
  if (ros::ok()) {
    warnx("caught signal, requesting shutdown");
    ros::shutdown();
  }
  else {
    errx(EXIT_SUCCESS, "caught signal (again?), attempting forced exit");
  }
}


namespace {
  
  
  class Servo
    : public TestUtilBase
  {
  public:
    shared_ptr<Skill> skill;    
    
    virtual int init(jspace::State const & state) {
      if (skill) {
	warnx("Servo::init(): already initialized");
	return -1;
      }
      if (factory->getSkillTable().empty()) {
	warnx("Servo::init(): empty skill table");
	return -2;
      }
      if ( ! model) {
	warnx("Servo::init(): no model");
	return -3;
      }
      
      if(!model->setConstraint("Dreamer_Base")) {
	warnx("Servo::init(): model->setConstraint() failed: Is the constraint defined?");
	return -4;
      }

      model->update(state);
    
      jspace::Status status(controller->init(*model));
      if ( ! status) {
	warnx("Servo::init(): controller->init() failed: %s", status.errstr.c_str());
	return -5;
      }
      
      skill = factory->getSkillTable()[0]; // XXXX to do: allow selection at runtime
      status = skill->init(*model);
      if ( ! status) {
	warnx("Servo::init(): skill->init() failed: %s", status.errstr.c_str());
	skill.reset();
	return -6;
      }
      
	  destination = Vector::Zero(3);
      return 0;
    }
    
    
    virtual int update(jspace::State const & state,
		       jspace::Vector & command)
    {
      if ( ! skill) {
	warnx("Servo::update(): not initialized\n");
	return -1;
      }
      
	  destination(0) = cmd.buf[0] * pow(10., cmd.exp[0]);
	  destination(1) = cmd.buf[1] * pow(10., cmd.exp[1]);
	  destination(2) = cmd.buf[2] * pow(10., cmd.exp[2]);

      model->update(state);
      
      jspace::Status status(controller->computeCommand(*model, *skill, command));
      if ( ! status) {
	warnx("Servo::update(): controller->computeCommand() failed: %s", status.errstr.c_str());
	return -2;
      }
      
      return 0;
    }
    
    
    virtual int cleanup(void)
    {
      skill.reset();
      return 0;
    }
    
    
    virtual int slowdown(long long iteration,
			 long long desired_ns,
			 long long actual_ns)
    {
      actual_servo_rate = 1000000000 / actual_ns;
      return 0;
    }
  };
  
}


int main(int argc, char ** argv)
{
  pthread_t udp_receiver;
  pthread_t ip_notifier;

  struct sigaction sa;
  bzero(&sa, sizeof(sa));
  sa.sa_handler = handle;
  if (0 != sigaction(SIGINT, &sa, 0)) {
    err(EXIT_FAILURE, "sigaction");
  }
  
  // Before we attempt to read any tasks and skills from the YAML
  // file, we need to inform the static type registry about custom
  // additions such as the HelloGoodbyeSkill.
  Factory::addSkillType<uta_opspace::HelloGoodbyeSkill>("uta_opspace::HelloGoodbyeSkill");
  Factory::addSkillType<uta_opspace::TaskOriPostureSkill>("uta_opspace::TaskPostureSkill");
  Factory::addSkillType<uta_opspace::JointMultiPos>("uta_opspace::JointMultiPos");
  Factory::addSkillType<uta_opspace::CartMultiPos>("uta_opspace::CartMultiPos");
  Factory::addSkillType<uta_opspace::BaseMultiPos>("uta_opspace::BaseMultiPos");
  
  
  ros::init(argc, argv, "wbc_m3_ctrl_servo", ros::init_options::NoSigintHandler);
  parse_options(argc, argv);
  ros::NodeHandle node("~");
  
  controller.reset(new ControllerNG("wbc_m3_ctrl::servo"));
  param_cbs.reset(new ParamCallbacks());

  pthread_create( &udp_receiver, NULL, receive_udp, (void*)&cmd);
  pthread_create( &ip_notifier, NULL, notify_thread, (void*)1);

  Servo servo;
  try {
    if (verbose) {
      warnx("initializing param callbacks");
    }
    registry.reset(factory->createRegistry());
    registry->add(controller);
    param_cbs->init(node, registry, 1, 100);
    
    if (verbose) {
      warnx("starting servo with %lld Hz", servo_rate);
    }
    actual_servo_rate = servo_rate;
    servo.start(servo_rate);
  }
  catch (std::runtime_error const & ee) {
    errx(EXIT_FAILURE, "failed to start servo: %s", ee.what());
  }
  
  warnx("started servo RT thread");
  ros::Time dbg_t0(ros::Time::now());
  ros::Time dump_t0(ros::Time::now());
  ros::Duration dbg_dt(0.1);
  ros::Duration dump_dt(0.05);
  
  message *pMsg = allocMessage(10);
  pMsg->count = 10;
  while (ros::ok()) {
    ros::Time t1(ros::Time::now());
    if (verbose) {
      if (t1 - dbg_t0 > dbg_dt) {
	dbg_t0 = t1;
	servo.skill->dbg(cout, "\n\n**************************************************", "");
	controller->dbg(cout, "--------------------------------------------------", "");
	cout << "--------------------------------------------------\n";
	Vector jpos = model->getFullState().position_;
	Vector gamma = controller->getCommand();
	jspace::pretty_print(jpos, cout, "jpos", "  ");
	jspace::pretty_print(model->getFullState().velocity_, cout, "jvel", "  ");
	jspace::pretty_print(model->getState().force_, cout, "jforce", "  ");
	Matrix ori_mtx(model->getState().orientation_mtx_);
	cout << "Ori_mtx:\n";
	for (size_t ii(0); ii < ori_mtx.rows(); ++ii) {
	  for (size_t jj(0); jj < ori_mtx.cols(); ++jj) {
	    cout << ori_mtx(ii,jj) << "    ";
	  }
	  cout <<"\n";
	}
	cout << "\n";
	jspace::pretty_print(gamma, cout, "gamma", "  ");
	Vector gravity;
	model->getGravity(gravity);
	jspace::pretty_print(gravity, cout, "gravity", "  ");
	cout << "servo rate: " << actual_servo_rate << "\n";	
	
	pMsg->timeStamp = (long long )(t1.toSec() * 1000000.);
	pMsg->data[0] = jpos(0);
	pMsg->data[1] = jpos(1);
	pMsg->data[2] = jpos(2);
	pMsg->data[3] = gamma(0);
	pMsg->data[4] = gamma(1);
	pMsg->data[5] = gamma(2);
	send_udp(pMsg);
      }
    }
#if 0
    if (t1 - dump_t0 > dump_dt) {
      dump_t0 = t1;
      controller->qhlog(*servo.skill, rt_get_cpu_time_ns() / 1000);
    }
#endif
    ros::spinOnce();
    usleep(10000);		// 100Hz-ish
  }
  
  warnx("shutting down");
  servo.shutdown();
}
