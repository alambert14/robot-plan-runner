#include "drake_lcmtypes/drake/lcmt_robot_state.hpp"

#include "admittance_trajectory_plan.h"

using drake::manipulation::planner::DifferentialInverseKinematicsResult;
using drake::manipulation::planner::DifferentialInverseKinematicsStatus;
using drake::manipulation::planner::internal::DoDifferentialInverseKinematics;
using drake::manipulation::planner::ComputePoseDiffInCommonFrame;
using drake::Vector3;
using drake::Vector6;
using drake::MatrixX;

using std::cout;
using std::endl;

using std::sin;
using std::cos;

void AdmittanceTrajectoryPlan::Step(const State &state, double control_period,
                                   double t, Command *cmd) const {

  // 1. Update diffik mbp with the current status of the robot.
  plant_->SetPositions(plant_context_.get(), state.q);

  // 2. Query reference position and actual position of tool frame.
  const drake::math::RigidTransformd X_WTr(quat_traj_.orientation(t),
                                           xyz_traj_.value(t));
  const auto& frame_W = plant_->world_frame();
  const auto X_WE = plant_->CalcRelativeTransform(
      *plant_context_, frame_W, frame_E_);
  const auto X_WT = X_WE * X_ET_;

  // 3. Convert F_TC_ into F_TrC
  auto R_TrT = (X_WTr.inverse() * X_WT).rotation();
  // Adjoint transform for converting force.
  auto tau_TrC = R_TrT * F_TC_.head(3);
  auto f_TrC = R_TrT * F_TC_.tail(3);
  auto xyzdot_TrC = R_TrT * V_TC_.tail(3);

  // Converting rpydot from one frame to another requires going through angular 
  // velocity. This involves some work unfortunately....

  auto rpy_TC = drake::math::RollPitchYaw<double>(X_TC_.rotation());
  double r_TC = rpy_TC.roll_angle();
  double p_TC = rpy_TC.pitch_angle();
  double y_TC = rpy_TC.yaw_angle();

  Eigen::Matrix3d Ninv;
  Ninv << cos(p_TC) * cos(y_TC), -sin(y_TC), 0, 
          cos(p_TC) * sin(y_TC), cos(y_TC), 0,
          -sin(p_TC), 0, 1;

  auto omega_TC = Ninv * V_TC_.head(3);
  auto omega_TrC = R_TrT * omega_TC;

  // convert omega to rpydot on TrC. requires rpy of TrC.
  auto rpy_TrC = drake::math::RollPitchYaw<double>(R_TrT * X_TC_.rotation());
  double r_TrC = rpy_TrC.roll_angle();
  double p_TrC = rpy_TrC.pitch_angle();
  double y_TrC = rpy_TrC.yaw_angle();

  Eigen::Matrix3d N;
  N << cos(y_TrC) / cos(p_TrC), sin(y_TrC) / cos(p_TrC), 0,
       -sin(y_TrC), cos(y_TrC), 0,
       cos(y_TrC) * tan(p_TrC), sin(y_TrC) * tan(p_TrC), 1;

  auto rpydot_TrC = N * omega_TrC;

  // 4. Using F_TrC, Compute the corrected X_TrC_des.

  // xyz_TrC_des is computed by Kxyz^{-1}(F_TrC - Dxyz * V_TrC)
  Eigen::Vector3d xyz_TrC_des;
  for (int i = 0; i < 3; i ++) {
    xyz_TrC_des[i] = (1./ Kxyz_[i]) * (f_TrC[i] - Dxyz_[i] * xyzdot_TrC[i]);
  }

  // rpy_TrC_des is computed using inverse bushing.
  // yaw. using w to avoid repetition with y coordinate.
  double w_TrC_des = (1./ Krpy_[2]) * (tau_TrC[2] - Drpy_[2] * rpydot_TrC[2]);
  double p_TrC_des = (1./ Krpy_[1]) * (
    tau_TrC[1] * cos(w_TrC_des) - tau_TrC[0] * sin(w_TrC_des) - Drpy_[1] * rpydot_TrC[1]);
  double r_TrC_des = (1./ Krpy_[0]) * (cos(p_TrC_des) * (
    tau_TrC[0] * cos(w_TrC_des) + tau_TrC[1] * sin(w_TrC_des)) - 
    tau_TrC[2] * sin(p_TrC_des) - Drpy_[0] * rpydot_TrC[0]);

  // Compute rigid transforms based on both components.
  drake::math::RollPitchYaw<double> rpy_TrC_des(
    r_TrC_des, p_TrC_des, w_TrC_des);

  drake::math::RigidTransformd X_TrC_des(
    drake::math::RotationMatrixd(rpy_TrC_des), xyz_TrC_des);

  // The way to think about this equation:
  // 1. The user should feel a force of lambda = X_TC should be preserved.
  // 2. the user should be feeling more displacement = X_wT should move more.
  auto X_WT_corrected = X_WTr * X_TrC_des * X_TC_.inverse();

  // A factor of 0.1 is multiplied because X_WT_corrected is updated from 
  // relative pose, which is published at ~20Hz. Since the robot is sending
  // q_cmd at 200Hz, we apply a zero-order hold this way.

  const Vector6<double> V_WT_desired =
      0.1 * ComputePoseDiffInCommonFrame(
          X_WT.GetAsIsometry3(), X_WT_corrected.GetAsIsometry3()) /
          params_->get_timestep();

  MatrixX<double> J_WT(6, plant_->num_velocities());
  plant_->CalcJacobianSpatialVelocity(*plant_context_,
                                    drake::multibody::JacobianWrtVariable::kV,
                                    frame_E_, X_ET_.translation(),
                                    frame_W, frame_W, &J_WT);


  DifferentialInverseKinematicsResult result = DoDifferentialInverseKinematics(
      state.q, state.v, X_WT, J_WT,
      drake::multibody::SpatialVelocity<double>(V_WT_desired), *params_);

  // 3. Check for errors and integrate.
  if (result.status != DifferentialInverseKinematicsStatus::kSolutionFound) {
    // Set the command to NAN so that state machine will detect downstream and
    // go to error state.
    cmd->q_cmd = NAN * Eigen::VectorXd::Zero(7);
    // TODO(terry-suh): how do I tell the use that the state machine went to
    // error because of this precise reason? Printing the error message here
    // seems like a good start, but we'll need to handle this better.
    std::cout << "DoDifferentialKinematics Failed to find a solution."
              << std::endl;
  } else {
    cmd->q_cmd = state.q + control_period * result.joint_velocities.value();
    cmd->tau_cmd = Eigen::VectorXd::Zero(7);
  }
}

AdmittanceTrajectoryPlan::~AdmittanceTrajectoryPlan() {
  is_running_ = false;
  // Wait for all threads to terminate.
  for (auto &a : threads_) {
    if (a.second.joinable()) {
      a.second.join();
    }
  }
}

void AdmittanceTrajectoryPlan::SubscribeForceTorque() {
  auto sub = lcm_->subscribe("FT",
    &AdmittanceTrajectoryPlan::HandleForceTorqueStatus, this);
  sub->setQueueCapacity(1);
  while(true) {
    if (lcm_->handleTimeout(10) < 0) {
      break;
    }
    if (!is_running_) {
      break;
    }
  }
}

void AdmittanceTrajectoryPlan::SubscribeVelocity() {
  auto sub = lcm_->subscribe("RELATIVE_VELOCITY",
    &AdmittanceTrajectoryPlan::HandleVelocityStatus, this);
  sub->setQueueCapacity(1);
  while(true) {
    if (lcm_->handleTimeout(10) < 0) {
      break;
    }
    if (!is_running_) {
      break;
    }    
  }
}

void AdmittanceTrajectoryPlan::SubscribePose() {
  auto sub = lcm_->subscribe("RELATIVE_POSE",
    &AdmittanceTrajectoryPlan::HandlePoseStatus, this);
  sub->setQueueCapacity(1);
  while(true) {
    if (lcm_->handleTimeout(10) < 0) {
      break;
    }
    if (!is_running_) {
      break;
    }    
  }
}

void AdmittanceTrajectoryPlan::HandleForceTorqueStatus(
  const lcm::ReceiveBuffer *, const std::string &channel,
  const drake::lcmt_robot_state *status_msg) {

    const int num_vars = (*status_msg).num_joints;
    const std::vector<float> data = (*status_msg).joint_position;
    auto data_eigen = Eigen::Map<const Eigen::VectorXf>(data.data(), num_vars);
    F_TC_ = data_eigen.cast<double>();
}      


void AdmittanceTrajectoryPlan::HandleVelocityStatus(
  const lcm::ReceiveBuffer *, const std::string &channel,
  const drake::lcmt_robot_state *status_msg) {

    const int num_vars = (*status_msg).num_joints;
    const std::vector<float> data = (*status_msg).joint_position;
    auto data_eigen = Eigen::Map<const Eigen::VectorXf>(data.data(), num_vars);
    V_TC_ = data_eigen.cast<double>();    
}

void AdmittanceTrajectoryPlan::HandlePoseStatus(
  const lcm::ReceiveBuffer *, const std::string &channel,
  const drake::lcmt_robot_state *status_msg) {

    const int num_vars = (*status_msg).num_joints;
    const std::vector<float> data = (*status_msg).joint_position;
    auto data_eigen = Eigen::Map<const Eigen::VectorXf>(data.data(), num_vars);
    const auto X_TC = data_eigen.cast<double>();
    const Eigen::Quaterniond q_TC(X_TC[0], X_TC[1], X_TC[2], X_TC[3]);
    X_TC_ = drake::math::RigidTransformd(q_TC, X_TC.tail(3));
}
