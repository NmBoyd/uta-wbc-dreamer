/*
 * ROS support for Stanford-WBC http://stanford-wbc.sourceforge.net/
 *
 * Copyright (c) 2010 Stanford University. All rights reserved.
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
   \file jspace/ros/Model.cpp
   \author Roland Philippsen
*/

#include "Model.hpp"
#include "urdf_to_tao.hpp"
#include "urdf_dump.hpp"
#include <urdf/model.h>
#include <jspace/tao_dump.hpp>
#include <tao/dynamics/taoNode.h>
#include <XmlRpcValue.h>
#include <XmlRpcException.h>

using namespace std;


namespace jspace {
  namespace ros {  
    
    // rosconsole (and maybe others) do not really like to live in a nested namespace
    using namespace ::ros;

  Model::
  Model(std::string const & param_prefix)
    : tao_root_param_name_(param_prefix + "tao_root_name"),
      active_links_param_name_(param_prefix + "active_links"),
      gravity_compensated_links_param_name_(param_prefix + "gravity_compensated_links")
  {
    gravity_[0] = 0;
    gravity_[1] = 0;
    gravity_[2] = -9.81;
  }
  
  
  Model::
  ~Model()
  {
    for (size_t ii(0); ii < tao_trees_.size(); ++ii) {
      delete tao_trees_[ii];
    }
  }
    
    
    void Model::
    parseGravityCompensatedLinks(::ros::NodeHandle & nn,
				 std::string gc_links_param_name,
				 std::vector<std::string> & gc_links,
				 ActiveLinkFilter const * opt_link_filter)
      throw(std::runtime_error)
    {
      XmlRpc::XmlRpcValue gc_links_value;
      if ( ! nn.getParam(gc_links_param_name, gc_links_value)) {
	ROS_WARN ("jspace::ros::Model::parseGravityCompensatedLinks():"
		  " no parameter called `%s' (skipping gravity compensation)",
		  gc_links_param_name.c_str());
	return;
      }
      
      try {
	std::string foo;
	for (int ii(0); ii < gc_links_value.size(); ++ii) {
	  foo = static_cast<std::string const &>(gc_links_value[ii]);
	  if (( ! opt_link_filter) || opt_link_filter->HaveLink(foo)) {
	    gc_links.push_back(foo);
	    ROS_INFO ("flagging link `%s' as gravity compensated", foo.c_str());
	  }
	  else {
	    ROS_WARN ("link `%s' not active, cannot flag it as gravity compensated", foo.c_str());
	  }
	}
      }
      catch (XmlRpc::XmlRpcException const & ee) {
	std::ostringstream msg;
	msg << "jspace::ros::Model::initFromURDF():"
	    << " XmlRpcException while reading gravity compensated links: "
	    << ee.getMessage();
	throw std::runtime_error(msg.str());
      }
    }
    
    
  void Model::
  initFromURDF(::ros::NodeHandle &nn, urdf::Model const & urdf,
	       size_t n_tao_trees) throw(std::runtime_error)
  {
    if ( ! tao_trees_.empty()) {
      throw std::runtime_error("jspace::ros::Model::initFromURDF(): already (partially?) initialized");
    }
    
    if (1 > n_tao_trees) {
      ROS_WARN ("resizing n_tao_trees to one");
      n_tao_trees = 1;
    }
    
    if ( ! nn.getParam(tao_root_param_name_, tao_root_name_)) {
      throw std::runtime_error("jspace::ros::Model::initFromURDF(): invalid tao_root_param_name_ \""
			       + tao_root_param_name_ + "\"");
    }
    ROS_INFO ("tao_root_name_ is `%s'", tao_root_name_.c_str());
    
    XmlRpc::XmlRpcValue active_links_value;
    if ( ! nn.getParam(active_links_param_name_, active_links_value)) {
      throw std::runtime_error("jspace::ros::Model::initFromURDF(): invalid active_links_param_name_ \""
			       + active_links_param_name_ + "\"");
    }
    
    ActiveLinkFilter link_filter;
    link_filter.AddLink(tao_root_name_);
    try {
      std::string foo;
      for (int ii(0); ii < active_links_value.size(); ++ii) {
	foo = static_cast<std::string const &>(active_links_value[ii]);
	link_filter.AddLink(foo);
	ROS_INFO ("added active link `%s'", foo.c_str());
      }
    }
    catch (XmlRpc::XmlRpcException const & ee) {
      std::ostringstream msg;
      msg << "jspace::ros::Model::initFromURDF():"
	  << " XmlRpcException while reading active links: "
	  << ee.getMessage();
      throw std::runtime_error(msg.str());
    }
    
    convert_urdf_to_tao_n(urdf,
			  tao_root_name_,
			  link_filter,
			  tao_trees_,
			  n_tao_trees);
    /* XXXX to do: if info is enabled... */ {
      std::ostringstream msg;
      msg << "converted URDF to TAO\n";
      dump_tao_tree_info(msg, tao_trees_[0], "  ", false);
      ROS_INFO ("%s", msg.str().c_str());
    }
    
    parseGravityCompensatedLinks(nn, gravity_compensated_links_param_name_,
				 gravity_compensated_links_, &link_filter);
    
    if (tao_trees_[0]->info.empty()) {
      ostringstream msg;
      msg << "jspace::ros::Model::initFromURDF(): no nodes in the TAO tree\n"
	  << "  urdf:\n";
      dump_urdf_tree(msg, urdf, "    ", false);
      msg << "  TAO root name: " << tao_root_name_ << "\n";
      throw runtime_error(msg.str());
    }
  }
  
  
  void Model::
  initFromParam(::ros::NodeHandle &nn, std::string const & urdf_param_name,
		size_t n_tao_trees) throw(std::runtime_error)
  {
    std::string urdf_string;
    if ( ! nn.getParam(urdf_param_name, urdf_string)) {
      throw runtime_error("jspace::ros::Model::initFromParam(): invalid urdf_param_name \""
			  + urdf_param_name + "\"");
    }
    TiXmlDocument urdf_xml;
    urdf_xml.Parse(urdf_string.c_str());
    TiXmlElement * urdf_root(urdf_xml.FirstChildElement("robot"));
    if ( ! urdf_root) {
      throw runtime_error("jspace::ros::Model::initFromParam(): no <robot> element in urdf_param_name \""
			  + urdf_param_name + "\"");
    }
    urdf::Model urdf_model;
    if ( ! urdf_model.initXml(urdf_root)) {
      throw runtime_error("jspace::ros::Model::initFromParam(): initXml() failed on urdf_param_name \""
			  + urdf_param_name + "\"");
    }
    initFromURDF(nn, urdf_model, n_tao_trees);
  }
  
}
}
