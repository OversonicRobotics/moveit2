/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2018 Pilz GmbH & Co. KG
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Pilz GmbH & Co. KG nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

#include <boost/core/demangle.hpp>
#include <gtest/gtest.h>

#include <moveit/planning_interface/planning_interface.hpp>
#include <moveit/planning_scene/planning_scene.hpp>
#include <moveit/robot_model/robot_model.hpp>
#include <moveit/robot_model_loader/robot_model_loader.hpp>
#include <moveit/robot_state/conversions.hpp>
#include <moveit/robot_state/robot_state.hpp>

#include <pilz_industrial_motion_planner/joint_limits_aggregator.hpp>
#include <pilz_industrial_motion_planner/joint_limits_container.hpp>
#include <pilz_industrial_motion_planner/trajectory_generator_circ.hpp>
#include <pilz_industrial_motion_planner/trajectory_generator_lin.hpp>
#include <pilz_industrial_motion_planner/trajectory_generator_ptp.hpp>
#include <pilz_industrial_motion_planner/trajectory_blender_transition_window.hpp>

#include "test_utils.hpp"

#include <rclcpp/rclcpp.hpp>

#include <cartesian_limits_parameters.hpp>

const std::string PARAM_MODEL_NO_GRIPPER_NAME{ "robot_description" };
const std::string PARAM_MODEL_WITH_GRIPPER_NAME{ "robot_description_pg70" };
const std::string PARAM_NAMESPACE_LIMITS{ "robot_description_planning" };

/**
 * A value type container to combine type and value
 * In the tests types are trajectory generators.
 * value = 0 refers to robot model without gripper
 * value = 1 refers to robot model with gripper
 */
template <typename T, int N>
class ValueTypeContainer
{
public:
  typedef T Type_;
  static const int VALUE = N;
};
template <typename T, int N>
const int ValueTypeContainer<T, N>::VALUE;

typedef ValueTypeContainer<pilz_industrial_motion_planner::TrajectoryGeneratorPTP, 0> PTP_NO_GRIPPER;
typedef ValueTypeContainer<pilz_industrial_motion_planner::TrajectoryGeneratorPTP, 1> PTP_WITH_GRIPPER;
typedef ValueTypeContainer<pilz_industrial_motion_planner::TrajectoryGeneratorLIN, 0> LIN_NO_GRIPPER;
typedef ValueTypeContainer<pilz_industrial_motion_planner::TrajectoryGeneratorLIN, 1> LIN_WITH_GRIPPER;
typedef ValueTypeContainer<pilz_industrial_motion_planner::TrajectoryGeneratorCIRC, 0> CIRC_NO_GRIPPER;
typedef ValueTypeContainer<pilz_industrial_motion_planner::TrajectoryGeneratorCIRC, 1> CIRC_WITH_GRIPPER;

typedef ::testing::Types<PTP_NO_GRIPPER, PTP_WITH_GRIPPER, LIN_NO_GRIPPER, LIN_WITH_GRIPPER, CIRC_NO_GRIPPER,
                         CIRC_WITH_GRIPPER>
    TrajectoryGeneratorCommonTestTypes;

typedef ::testing::Types<PTP_NO_GRIPPER, LIN_NO_GRIPPER, CIRC_NO_GRIPPER> TrajectoryGeneratorCommonTestTypesNoGripper;

typedef ::testing::Types<PTP_WITH_GRIPPER, LIN_WITH_GRIPPER, CIRC_WITH_GRIPPER>
    TrajectoryGeneratorCommonTestTypesWithGripper;

/**
 * type parameterized test fixture
 */
template <typename T>
class TrajectoryGeneratorCommonTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    rclcpp::NodeOptions node_options;
    node_options.automatically_declare_parameters_from_overrides(true);
    node_ = rclcpp::Node::make_shared("unittest_trajectory_generator_common", node_options);

    // load robot model
    // const std::string robot_description_param = (!T::VALUE ? PARAM_MODEL_NO_GRIPPER_NAME : PARAM_MODEL_WITH_GRIPPER_NAME);
    robot_model_loader::RobotModelLoader rm_loader(node_, PARAM_MODEL_NO_GRIPPER_NAME);
    robot_model_ = rm_loader.getModel();
    ASSERT_TRUE(bool(robot_model_)) << "Failed to load robot model";
    planning_scene_ = std::make_shared<planning_scene::PlanningScene>(robot_model_);

    // get parameters
    ASSERT_TRUE(node_->has_parameter("planning_group"));
    node_->get_parameter<std::string>("planning_group", planning_group_);
    ASSERT_TRUE(node_->has_parameter("target_link"));
    node_->get_parameter<std::string>("target_link", target_link_);

    testutils::checkRobotModel(robot_model_, planning_group_, target_link_);

    // create the limits container
    pilz_industrial_motion_planner::JointLimitsContainer joint_limits =
        pilz_industrial_motion_planner::JointLimitsAggregator::getAggregatedLimits(
            node_, PARAM_NAMESPACE_LIMITS, robot_model_->getActiveJointModels());

    cartesian_limits::Params cart_limits;
    cart_limits.max_trans_vel = 0.5 * M_PI;
    cart_limits.max_trans_acc = 2;
    cart_limits.max_trans_dec = 2;
    cart_limits.max_rot_vel = 1;

    pilz_industrial_motion_planner::LimitsContainer planner_limits;
    planner_limits.setJointLimits(joint_limits);
    planner_limits.setCartesianLimits(cart_limits);

    // create planner instance
    trajectory_generator_ = std::make_unique<typename T::Type_>(robot_model_, planner_limits, planning_group_);
    ASSERT_NE(nullptr, trajectory_generator_) << "failed to create trajectory generator";

    // create a valid motion plan request with goal in joint space as basis for
    // tests
    req_.group_name = planning_group_;
    req_.max_velocity_scaling_factor = 1.0;
    req_.max_acceleration_scaling_factor = 1.0;
    moveit::core::RobotState rstate(robot_model_);
    rstate.setToDefaultValues();
    rstate.setJointGroupPositions(planning_group_, std::vector<double>{ 0, M_PI / 2, 0, M_PI / 2, 0, 0 });
    rstate.setVariableVelocities(std::vector<double>(rstate.getVariableCount(), 0.0));
    moveit::core::robotStateToRobotStateMsg(rstate, req_.start_state, false);
    moveit_msgs::msg::Constraints goal_constraint;
    moveit_msgs::msg::JointConstraint joint_constraint;
    joint_constraint.joint_name = this->robot_model_->getActiveJointModels().front()->getName();
    joint_constraint.position = 0.5;
    goal_constraint.joint_constraints.push_back(joint_constraint);
    req_.goal_constraints.push_back(goal_constraint);
  }

protected:
  // ros stuff
  rclcpp::Node::SharedPtr node_;
  moveit::core::RobotModelConstPtr robot_model_;
  planning_scene::PlanningSceneConstPtr planning_scene_;

  // trajectory generator
  std::unique_ptr<pilz_industrial_motion_planner::TrajectoryGenerator> trajectory_generator_;
  planning_interface::MotionPlanResponse res_;
  planning_interface::MotionPlanRequest req_;
  // test parameters from parameter server
  std::string planning_group_, target_link_;
};
// Define the types we need to test
TYPED_TEST_SUITE(TrajectoryGeneratorCommonTest, TrajectoryGeneratorCommonTestTypes, /* ... */);

template <typename T>
class TrajectoryGeneratorCommonTestNoGripper : public TrajectoryGeneratorCommonTest<T>
{
};
TYPED_TEST_SUITE(TrajectoryGeneratorCommonTestNoGripper, TrajectoryGeneratorCommonTestTypesNoGripper, /* ... */);

// TODO(henningkayser):Enable tests with gripper support
// template <typename T>
// class TrajectoryGeneratorCommonTestWithGripper : public TrajectoryGeneratorCommonTest<T>
// {
// };
// TYPED_TEST_SUITE(TrajectoryGeneratorCommonTestWithGripper, TrajectoryGeneratorCommonTestTypesWithGripper, /* ... */);

/**
 * @brief test invalid scaling factor. The scaling factor must be in the range
 * of [0.0001, 1]
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, InvalideScalingFactor)
{
  this->req_.max_velocity_scaling_factor = 2.0;
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_MOTION_PLAN);

  this->req_.max_velocity_scaling_factor = 1.0;
  this->req_.max_acceleration_scaling_factor = 0;
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_MOTION_PLAN);

  this->req_.max_velocity_scaling_factor = 0.00001;
  this->req_.max_acceleration_scaling_factor = 1;
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_MOTION_PLAN);

  this->req_.max_velocity_scaling_factor = 1;
  this->req_.max_acceleration_scaling_factor = -1;
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_MOTION_PLAN);
}

/**
 * @brief Test invalid motion plan request for all trajectory generators
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, InvalidGroupName)
{
  this->req_.group_name = "foot";
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(moveit_msgs::msg::MoveItErrorCodes::INVALID_GROUP_NAME, this->res_.error_code.val);
}

/**
 * @brief Test invalid motion plan request for all trajectory generators
 */
TYPED_TEST(TrajectoryGeneratorCommonTestNoGripper, GripperGroup)
{
  this->req_.group_name = "gripper";
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(moveit_msgs::msg::MoveItErrorCodes::INVALID_GROUP_NAME, this->res_.error_code.val);
}

/**
 * @brief Test invalid motion plan request for all trajectory generators
 */
// TYPED_TEST(TrajectoryGeneratorCommonTestWithGripper, GripperGroup)
// {
//   this->req_.group_name = "gripper";
//   this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
//   EXPECT_EQ(moveit_msgs::msg::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS, this->res_.error_code.val);
// }

/**
 * @brief Test if there is a valid inverse kinematics solver for this planning
 * group
 * You can only test this case by commenting the planning_context.launch in the
 * .test file
 * //TODO create a separate robot model without ik solver and use it to create a
 * trajectory generator
 */
// TYPED_TEST(TrajectoryGeneratorCommonTest, NoIKSolver)
//{
//  EXPECT_FALSE(this->trajectory_generator_->generate(this->req_, this->res_));
//  EXPECT_EQ(this->res_.error_code.val,
//  moveit_msgs::msg::MoveItErrorCodes::INVALID_GROUP_NAME);
//}

/**
 * @brief test the case of empty joint names in start state
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, EmptyJointNamesInStartState)
{
  this->req_.start_state.joint_state.name.clear();
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_ROBOT_STATE);
}

/**
 * @brief size of joint name and joint position does not match in start state
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, InconsistentStartState)
{
  this->req_.start_state.joint_state.name.push_back("joint_7");
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_ROBOT_STATE);
}

/**
 * @brief joint position out of limit in start state
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, StartPostionOutOfLimit)
{
  this->req_.start_state.joint_state.position[0] = 100;
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_ROBOT_STATE);
}

/**
 * @brief Check that no trajectory is generated if a start velocity is given
 *
 * @note This test is here for regression, however in general generators that
 * can work with a given
 * start velocity are highly desired.
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, StartPositionVelocityNoneZero)
{
  this->req_.start_state.joint_state.velocity[0] = 100;
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_ROBOT_STATE);
}

/**
 * @brief goal constraints is empty
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, EmptyGoalConstraints)
{
  this->req_.goal_constraints.clear();
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS);
}

/**
 * @brief multiple goals
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, MultipleGoals)
{
  moveit_msgs::msg::JointConstraint joint_constraint;
  moveit_msgs::msg::PositionConstraint position_constraint;
  moveit_msgs::msg::OrientationConstraint orientation_constraint;
  moveit_msgs::msg::Constraints goal_constraint;

  // two goal constraints
  this->req_.goal_constraints.push_back(goal_constraint);
  this->req_.goal_constraints.push_back(goal_constraint);
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS);

  // one joint constraint and one orientation constraint
  goal_constraint.joint_constraints.push_back(joint_constraint);
  goal_constraint.orientation_constraints.push_back(orientation_constraint);
  this->req_.goal_constraints.clear();
  this->req_.goal_constraints.push_back(goal_constraint);
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS);

  // one joint constraint and one Cartesian constraint
  goal_constraint.position_constraints.push_back(position_constraint);
  goal_constraint.orientation_constraints.push_back(orientation_constraint);
  this->req_.goal_constraints.clear();
  this->req_.goal_constraints.push_back(goal_constraint);
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS);

  // two Cartesian constraints
  goal_constraint.joint_constraints.clear();
  goal_constraint.position_constraints.push_back(position_constraint);
  goal_constraint.orientation_constraints.push_back(orientation_constraint);
  goal_constraint.position_constraints.push_back(position_constraint);
  goal_constraint.orientation_constraints.push_back(orientation_constraint);
  this->req_.goal_constraints.clear();
  this->req_.goal_constraints.push_back(goal_constraint);
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS);
}

/**
 * @brief invalid joint name in joint constraint
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, InvalideJointNameInGoal)
{
  moveit_msgs::msg::JointConstraint joint_constraint;
  joint_constraint.joint_name = "test_joint_2";
  this->req_.goal_constraints.front().joint_constraints[0] = joint_constraint;
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS);
}

/**
 * @brief MissingJointConstraint
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, MissingJointConstraint)
{
  moveit_msgs::msg::JointConstraint joint_constraint;
  joint_constraint.joint_name = "test_joint_2";
  this->req_.goal_constraints.front().joint_constraints.pop_back();  //<-- Missing joint constraint
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS);
}

/**
 * @brief invalid joint position in joint constraint
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, InvalideJointPositionInGoal)
{
  this->req_.goal_constraints.front().joint_constraints[0].position = 100;
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS);
}

/**
 * @brief invalid link name in Cartesian goal constraint
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, InvalidLinkNameInCartesianGoal)
{
  moveit_msgs::msg::PositionConstraint position_constraint;
  moveit_msgs::msg::OrientationConstraint orientation_constraint;
  moveit_msgs::msg::Constraints goal_constraint;
  // link name not set
  goal_constraint.position_constraints.push_back(position_constraint);
  goal_constraint.orientation_constraints.push_back(orientation_constraint);
  this->req_.goal_constraints.clear();
  this->req_.goal_constraints.push_back(goal_constraint);
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS);

  // different link names in position and orientation goals
  goal_constraint.position_constraints.front().link_name = "test_link_1";
  goal_constraint.orientation_constraints.front().link_name = "test_link_2";
  this->req_.goal_constraints.clear();
  this->req_.goal_constraints.push_back(goal_constraint);
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS);

  // no solver for the link
  goal_constraint.orientation_constraints.front().link_name = "test_link_1";
  this->req_.goal_constraints.clear();
  this->req_.goal_constraints.push_back(goal_constraint);
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::NO_IK_SOLUTION);
}

/**
 * @brief no pose set in position constraint
 */
TYPED_TEST(TrajectoryGeneratorCommonTest, EmptyPrimitivePoses)
{
  moveit_msgs::msg::PositionConstraint position_constraint;
  moveit_msgs::msg::OrientationConstraint orientation_constraint;
  moveit_msgs::msg::Constraints goal_constraint;
  position_constraint.link_name =
      this->robot_model_->getJointModelGroup(this->planning_group_)->getLinkModelNames().back();
  orientation_constraint.link_name = position_constraint.link_name;

  goal_constraint.position_constraints.push_back(position_constraint);
  goal_constraint.orientation_constraints.push_back(orientation_constraint);
  this->req_.goal_constraints.clear();
  this->req_.goal_constraints.push_back(goal_constraint);
  this->trajectory_generator_->generate(this->planning_scene_, this->req_, this->res_);
  EXPECT_EQ(this->res_.error_code.val, moveit_msgs::msg::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS);
}

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
