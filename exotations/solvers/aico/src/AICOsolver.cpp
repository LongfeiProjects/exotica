/*
 *  Created on: 19 Apr 2014
 *      Author: Vladimir Ivan
 *
 *  This code is based on algorithm developed by Marc Toussaint
 *  M. Toussaint: Robot Trajectory Optimization using Approximate Inference. In Proc. of the Int. Conf. on Machine Learning (ICML 2009), 1049-1056, ACM, 2009.
 *  http://ipvs.informatik.uni-stuttgart.de/mlr/papers/09-toussaint-ICML.pdf
 *  Original code available at http://ipvs.informatik.uni-stuttgart.de/mlr/marc/source-code/index.html
 * 
 * Copyright (c) 2016, University Of Edinburgh 
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions are met: 
 * 
 *  * Redistributions of source code must retain the above copyright notice, 
 *    this list of conditions and the following disclaimer. 
 *  * Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *  * Neither the name of  nor the names of its contributors may be used to 
 *    endorse or promote products derived from this software without specific 
 *    prior written permission. 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 * POSSIBILITY OF SUCH DAMAGE. 
 *
 */

/** \file AICOsolver.h
 \brief Approximate Inference Control */

#include "aico/AICOsolver.h"

REGISTER_MOTIONSOLVER_TYPE("AICOsolver", exotica::AICOsolver)

namespace exotica
{
void AICOsolver::Instantiate(AICOsolverInitializer& init)
{
    std::string mode = init.SweepMode;
    if (mode == "Forwardly")
        sweepMode = smForwardly;
    else if (mode == "Symmetric")
        sweepMode = smSymmetric;
    else if (mode == "LocalGaussNewton")
        sweepMode = smLocalGaussNewton;
    else if (mode == "LocalGaussNewtonDamped")
        sweepMode = smLocalGaussNewtonDamped;
    else
    {
        throw_named("Unknown sweep mode '" << init.SweepMode << "'");
    }
    max_backtrack_iterations = init.MaxBacktrackIterations;
    minimum_step_tolerance = init.MinStep;
    step_tolerance = init.StepTolerance;
    function_tolerance = init.FunctionTolerance;
    damping_init = init.Damping;
    useBwdMsg = init.UseBackwardMessage;
}

void AICOsolver::saveCosts(std::string file_name)
{
    std::ofstream myfile;
    myfile.open(file_name.c_str());
    myfile << "Control Task";
    for (int t = 0; t < prob_->getT(); t++)
    {
        myfile << "\n"
               << costControl(t);
        myfile << " " << costTask(t);
    }
    myfile.close();
}

AICOsolver::AICOsolver()
    : damping(0.01),
      minimum_step_tolerance(1e-5),
      step_tolerance(1e-5),
      function_tolerance(1e-5),
      max_backtrack_iterations(10),
      useBwdMsg(false),
      bwdMsg_v(),
      bwdMsg_Vinv(),
      s(),
      Sinv(),
      v(),
      Vinv(),
      r(),
      R(),
      rhat(),
      b(),
      Binv(),
      q(),
      qhat(),
      s_old(),
      Sinv_old(),
      v_old(),
      Vinv_old(),
      r_old(),
      R_old(),
      rhat_old(),
      b_old(),
      Binv_old(),
      q_old(),
      qhat_old(),
      dampingReference(),
      cost(0.0),
      cost_old(0.0),
      cost_prev(std::numeric_limits<double>::max()),
      b_step(0.0),
      Winv(),
      sweep(0),
      sweepMode(0),
      W(),
      updateCount(0),
      damping_init(0.0),
      q_stat()
{
}

void AICOsolver::getStats(std::vector<SinglePassMeanCoviariance>& q_stat_)
{
    q_stat_ = q_stat;
}

AICOsolver::~AICOsolver()
{
    // If this is not empty, your code is bad and you should feel bad!
    // Whoop whoop whoop whoop ...
}

void AICOsolver::specifyProblem(PlanningProblem_ptr problem)
{
    if (problem->type() != "exotica::UnconstrainedTimeIndexedProblem")
    {
        throw_named("This solver can't use problem of type '" << problem->type() << "'!");
    }
    MotionSolver::specifyProblem(problem);
    prob_ = std::static_pointer_cast<UnconstrainedTimeIndexedProblem>(problem);

    initMessages();
}

void AICOsolver::Solve(Eigen::MatrixXd& solution)
{
    prob_->preupdate();
    prob_->resetCostEvolution(getNumberOfMaxIterations() + 1);
    prob_->terminationCriterion = TerminationCriterion::NotStarted;
    planning_time_ = -1;

    Eigen::VectorXd q0 = prob_->applyStartState();
    std::vector<Eigen::VectorXd> q_init = prob_->getInitialTrajectory();

    // If the initial value of the initial trajectory does not equal the start
    // state, assume that no initial guess is provided and fill the trajectory
    // with the start state
    if (!(q0 - q_init[0]).isMuchSmallerThan(1e-6))
    {
        HIGHLIGHT("AICO::Solve cold-started");
        q_init.assign(prob_->getT(), q0);
    }
    else
    {
        HIGHLIGHT("AICO::Solve called with initial trajectory guess");
    }

    prob_->setStartState(q_init[0]);
    prob_->applyStartState();

    // Check if the trajectory length has changed, if so update the messages.
    if (prob_->getT() != lastT) initMessages();

    Timer timer;
    if (debug_) ROS_WARN_STREAM("AICO: Setting up the solver");
    updateCount = 0;
    damping = damping_init;
    double d;
    if (!(prob_->getT() > 0))
    {
        throw_named("Problem has not been initialized properly: T=0!");
    }
    iterationCount = -1;
    initTrajectory(q_init);
    if (debug_) ROS_WARN_STREAM("AICO: Solving");

    // Reset sweep and iteration count
    sweep = 0;
    iterationCount = 0;
    while (iterationCount < getNumberOfMaxIterations())
    {
        // Check whether user interrupted (Ctrl+C)
        if (Server::isRos() && !ros::ok())
        {
            if (debug_) HIGHLIGHT("Solving cancelled by user");
            prob_->terminationCriterion = TerminationCriterion::UserDefined;
            break;
        }

        d = step();
        if (d < 0)
        {
            throw_named("Negative step size!");
        }

        // 0. Check maximum backtrack iterations
        if (damping && sweep >= max_backtrack_iterations)
        {
            if (debug_) HIGHLIGHT("Maximum backtrack iterations reached, exiting.");
            prob_->terminationCriterion = TerminationCriterion::BacktrackIterationLimit;
            break;
        }

        // Check stopping criteria
        if (iterationCount > 1)
        {
            // Check convergence if
            //    a) damping is on and the iteration has concluded (the sweep improved the cost)
            //    b) damping is off [each sweep equals one iteration]
            if (damping && sweepImprovedCost || !damping)
            {
                // 1. Check step tolerance
                // || x_t-x_t-1 || <= stepTolerance * max(1, || x_t ||)
                // TODO(#257): TODO(#256): move to Eigen::MatrixXd to make this easier to compute, in the meantime use old check
                //
                // TODO(#256): OLD TOLERANCE CHECK - TODO REMOVE
                if (d < minimum_step_tolerance)
                {
                    if (debug_) HIGHLIGHT("Satisfied tolerance\titer=" << iterationCount << "\td=" << d << "\tminimum_step_tolerance=" << minimum_step_tolerance);
                    prob_->terminationCriterion = TerminationCriterion::StepTolerance;
                    break;
                }

                // 2. Check function tolerance
                // (f_t-1 - f_t) <= functionTolerance * max(1, abs(f_t))
                if ((cost_prev - cost) <= function_tolerance * std::max(1.0, std::abs(cost)))
                {
                    if (debug_) HIGHLIGHT("Function tolerance achieved: " << (cost_prev - cost) << " <= " << function_tolerance * std::max(1.0, std::abs(cost)));
                    prob_->terminationCriterion = TerminationCriterion::FunctionTolerance;
                    break;
                }
                cost_prev = cost;
            }
        }
    }

    // Check whether maximum iteration count was reached
    if (iterationCount == getNumberOfMaxIterations())
    {
        if (debug_) HIGHLIGHT("Maximum iterations reached");
        prob_->terminationCriterion = TerminationCriterion::IterationLimit;
    }

    Eigen::MatrixXd sol(prob_->getT(), prob_->N);
    for (int tt = 0; tt < prob_->getT(); tt++)
    {
        sol.row(tt) = q[tt];
    }
    solution = sol;
    planning_time_ = timer.getDuration();
}

void AICOsolver::initMessages()
{
    if (prob_ == nullptr) throw_named("Problem definition is a NULL pointer!");

    if (prob_->N < 1)
    {
        throw_named("State dimension is too small: n=" << prob_->N);
    }
    if (prob_->getT() < 2)
    {
        throw_named("Number of time steps is too small: T=" << prob_->getT());
    }

    s.assign(prob_->getT(), Eigen::VectorXd::Zero(prob_->N));
    Sinv.assign(prob_->getT(), Eigen::MatrixXd::Zero(prob_->N, prob_->N));
    Sinv[0].diagonal().setConstant(1e10);
    v.assign(prob_->getT(), Eigen::VectorXd::Zero(prob_->N));
    Vinv.assign(prob_->getT(), Eigen::MatrixXd::Zero(prob_->N, prob_->N));
    if (useBwdMsg)
    {
        if (bwdMsg_v.rows() == prob_->N && bwdMsg_Vinv.rows() == prob_->N && bwdMsg_Vinv.cols() == prob_->N)
        {
            v[prob_->getT() - 1] = bwdMsg_v;
            Vinv[prob_->getT() - 1] = bwdMsg_Vinv;
        }
        else
        {
            useBwdMsg = false;
            WARNING("Backward message initialisation skipped, matrices have incorrect dimensions.");
        }
    }
    b.assign(prob_->getT(), Eigen::VectorXd::Zero(prob_->N));
    dampingReference.assign(prob_->getT(), Eigen::VectorXd::Zero(prob_->N));
    Binv.assign(prob_->getT(), Eigen::MatrixXd::Zero(prob_->N, prob_->N));
    Binv[0].setIdentity();
    Binv[0] = Binv[0] * 1e10;
    r.assign(prob_->getT(), Eigen::VectorXd::Zero(prob_->N));
    R.assign(prob_->getT(), Eigen::MatrixXd::Zero(prob_->N, prob_->N));
    rhat = Eigen::VectorXd::Zero(prob_->getT());
    qhat.assign(prob_->getT(), Eigen::VectorXd::Zero(prob_->N));
    linSolverTmp.resize(prob_->N, prob_->N);
    {
        q = b;
        if (prob_->W.rows() != prob_->N)
        {
            throw_named(prob_->W.rows() << "!=" << prob_->N);
        }
    }
    {
        // Set constant W,Win,H,Hinv
        W = prob_->W;
        Winv = W.inverse();
    }

    costControl.resize(prob_->getT());
    costControl.setZero();
    costTask.resize(prob_->getT());
    costTask.setZero();

    q_stat.resize(prob_->getT());
    for (int t = 0; t < prob_->getT(); t++)
    {
        q_stat[t].resize(prob_->N);
    }

    // Set lastT to the problem T
    lastT = prob_->getT();
}

void AICOsolver::initTrajectory(const std::vector<Eigen::VectorXd>& q_init)
{
    if (q_init.size() != prob_->getT())
    {
        throw_named("Incorrect number of timesteps provided!");
    }
    qhat = q_init;
    q = q_init;
    dampingReference = q_init;
    b = q_init;
    s = q_init;
    v = q_init;
    for (int t = 1; t < prob_->getT(); t++)
    {
        Sinv.at(t).setZero();
        Sinv.at(t).diagonal().setConstant(damping);
    }
    for (int t = 0; t < prob_->getT(); t++)
    {
        Vinv.at(t).setZero();
        Vinv.at(t).diagonal().setConstant(damping);
    }
    for (int t = 0; t < prob_->getT(); t++)
    {
        // Compute task message reference
        updateTaskMessage(t, b[t], 0.0);
    }

    cost = evaluateTrajectory(b, true);  // The problem will be updated via updateTaskMessage, i.e. do not update on this roll-out
    cost_prev = cost;
    prob_->setCostEvolution(0, cost);
    if (cost < 0) throw_named("Invalid cost! " << cost);
    if (debug_) HIGHLIGHT("Initial cost, updates: " << updateCount << ", cost(ctrl/task/total): " << costControl.sum() << "/" << costTask.sum() << "/" << cost);
    rememberOldState();
}

void AICOsolver::updateFwdMessage(int t)
{
    Eigen::MatrixXd barS(prob_->N, prob_->N), St;
    inverseSymPosDef(barS, Sinv[t - 1] + R[t - 1]);
    s[t] = barS * (Sinv[t - 1] * s[t - 1] + r[t - 1]);
    St = Winv + barS;
    inverseSymPosDef(Sinv[t], St);
}

void AICOsolver::updateBwdMessage(int t)
{
    Eigen::MatrixXd barV(prob_->N, prob_->N), Vt;
    if (t < prob_->getT() - 1)
    {
        inverseSymPosDef(barV, Vinv[t + 1] + R[t + 1]);
        v[t] = barV * (Vinv[t + 1] * v[t + 1] + r[t + 1]);
        Vt = Winv + barV;
        inverseSymPosDef(Vinv[t], Vt);
    }
    if (t == prob_->getT() - 1)
    {
        if (!useBwdMsg)
        {
            v[t] = b[t];
            Vinv[t].diagonal().setConstant(1);
        }
        else
        {
            v[prob_->getT() - 1] = bwdMsg_v;
            Vinv[prob_->getT() - 1] = bwdMsg_Vinv;
        }
    }
}

void AICOsolver::updateTaskMessage(int t,
                                   const Eigen::Ref<const Eigen::VectorXd>& qhat_t, double minimum_step_tolerance,
                                   double maxStepSize)
{
    Eigen::VectorXd diff = qhat_t - qhat[t];
    if ((diff.array().abs().maxCoeff() < minimum_step_tolerance)) return;
    double nrm = diff.norm();
    if (maxStepSize > 0. && nrm > maxStepSize)
    {
        qhat[t] += diff * (maxStepSize / nrm);
    }
    else
    {
        qhat[t] = qhat_t;
    }

    prob_->Update(qhat[t], t);
    updateCount++;
    double c = getTaskCosts(t);
    q_stat[t].addw(c > 0 ? 1.0 / (1.0 + c) : 1.0, qhat_t);
}

double AICOsolver::getTaskCosts(int t)
{
    double C = 0;
    Eigen::MatrixXd Jt;
    double prec;
    rhat[t] = 0;
    R[t].setZero();
    r[t].setZero();
    for (int i = 0; i < prob_->Cost.NumTasks; i++)
    {
        prec = prob_->Cost.Rho[t](i);
        if (prec > 0)
        {
            int start = prob_->Cost.Indexing[i].StartJ;
            int len = prob_->Cost.Indexing[i].LengthJ;
            Jt = prob_->Cost.J[t].middleRows(start, len).transpose();
            C += prec * (prob_->Cost.ydiff[t].segment(start, len)).squaredNorm();
            R[t] += prec * Jt * prob_->Cost.J[t].middleRows(start, len);
            r[t] += prec * Jt * (-prob_->Cost.ydiff[t].segment(start, len) + prob_->Cost.J[t].middleRows(start, len) * qhat[t]);
            rhat[t] += prec * (-prob_->Cost.ydiff[t].segment(start, len) + prob_->Cost.J[t].middleRows(start, len) * qhat[t]).squaredNorm();
        }
    }
    return prob_->ct * C;
}

void AICOsolver::updateTimeStep(int t, bool updateFwd, bool updateBwd,
                                int maxRelocationIterations, double minimum_step_tolerance, bool forceRelocation,
                                double maxStepSize)
{
    if (updateFwd) updateFwdMessage(t);
    if (updateBwd) updateBwdMessage(t);

    if (damping)
    {
        Binv[t] = Sinv[t] + Vinv[t] + R[t] + Eigen::MatrixXd::Identity(prob_->N, prob_->N) * damping;
        AinvBSymPosDef(b[t], Binv[t], Sinv[t] * s[t] + Vinv[t] * v[t] + r[t] + damping * dampingReference[t], linSolverTmp, prob_->N);
    }
    else
    {
        Binv[t] = Sinv[t] + Vinv[t] + R[t];
        AinvBSymPosDef(b[t], Binv[t], Sinv[t] * s[t] + Vinv[t] * v[t] + r[t], linSolverTmp, prob_->N);
    }

    for (int k = 0; k < maxRelocationIterations && !(Server::isRos() && !ros::ok()); k++)
    {
        if (!((!k && forceRelocation) || (b[t] - qhat[t]).array().abs().maxCoeff() > minimum_step_tolerance)) break;

        updateTaskMessage(t, b.at(t), 0., maxStepSize);

        //optional reUpdate fwd or bwd message (if the Dynamics might have changed...)
        if (updateFwd) updateFwdMessage(t);
        if (updateBwd) updateBwdMessage(t);

        if (damping)
        {
            Binv[t] = Sinv[t] + Vinv[t] + R[t] + Eigen::MatrixXd::Identity(prob_->N, prob_->N) * damping;
            AinvBSymPosDef(b[t], Binv[t], Sinv[t] * s[t] + Vinv[t] * v[t] + r[t] + damping * dampingReference[t], linSolverTmp, prob_->N);
        }
        else
        {
            Binv[t] = Sinv[t] + Vinv[t] + R[t];
            AinvBSymPosDef(b[t], Binv[t], Sinv[t] * s[t] + Vinv[t] * v[t] + r[t], linSolverTmp, prob_->N);
        }
    }
}

void AICOsolver::updateTimeStepGaussNewton(int t, bool updateFwd,
                                           bool updateBwd, int maxRelocationIterations, double minimum_step_tolerance,
                                           double maxStepSize)
{
    // TODO: implement updateTimeStepGaussNewton
    throw_named("Not implemented yet!");
}

double AICOsolver::evaluateTrajectory(const std::vector<Eigen::VectorXd>& x,
                                      bool skipUpdate)
{
    if (debug_) ROS_WARN_STREAM("Evaluating, iteration " << iterationCount << ", sweep " << sweep);
    Timer timer;
    double dSet, dUpd, dCtrl, dTask;

    q = x;
    dSet = timer.getDuration();

    // Perform update / roll-out
    if (!skipUpdate)
    {
        for (int t = 0; t < prob_->getT(); t++)
        {
            updateCount++;
            prob_->Update(q[t], t);
        }
    }
    dUpd += timer.getDuration();
    timer.reset();
    if (debug_ && !skipUpdate) HIGHLIGHT("Roll-out took: " << dUpd);

    for (int t = 1; t < prob_->getT(); t++)
    {
        timer.reset();
        if (Server::isRos() && !ros::ok()) return -1.0;

        // Control cost
        costControl(t) = prob_->getScalarTransitionCost(t);

        dCtrl += timer.getDuration();
        timer.reset();
        // Task cost
        costTask(t) = prob_->getScalarTaskCost(t);
        dTask += timer.getDuration();
    }

    cost = costControl.sum() + costTask.sum();
    return cost;
}

double AICOsolver::step()
{
    rememberOldState();
    int t;
    switch (sweepMode)
    {
        //NOTE: the dependence on (Sweep?..:..) could perhaps be replaced by (DampingReference.N?..:..)
        case smForwardly:
            for (t = 1; t < prob_->getT(); t++)
            {
                updateTimeStep(t, true, false, 1, minimum_step_tolerance, !iterationCount, 1.);  //relocate once on fwd Sweep
            }
            for (t = prob_->getT() - 2; t > 0; t--)
            {
                updateTimeStep(t, false, true, 0, minimum_step_tolerance, false, 1.);  //...not on bwd Sweep
            }
            break;
        case smSymmetric:
            // ROS_WARN_STREAM("Updating forward, iterationCount "<<iterationCount);
            for (t = 1; t < prob_->getT(); t++)
            {
                updateTimeStep(t, true, false, 1, minimum_step_tolerance, !iterationCount, 1.);  //relocate once on fwd & bwd Sweep
            }
            // ROS_WARN_STREAM("Updating backward, iterationCount "<<iterationCount);
            for (t = prob_->getT() - 2; t > 0; t--)
            {
                updateTimeStep(t, false, true, (iterationCount ? 1 : 0), minimum_step_tolerance, false, 1.);
            }
            break;
        case smLocalGaussNewton:
            for (t = 1; t < prob_->getT(); t++)
            {
                updateTimeStep(t, true, false, (iterationCount ? 5 : 1), minimum_step_tolerance, !iterationCount, 1.);  //relocate iteratively on
            }
            for (t = prob_->getT() - 2; t > 0; t--)
            {
                updateTimeStep(t, false, true, (iterationCount ? 5 : 0), minimum_step_tolerance, false, 1.);  //...fwd & bwd Sweep
            }
            break;
        case smLocalGaussNewtonDamped:
            for (t = 1; t < prob_->getT(); t++)
            {
                updateTimeStepGaussNewton(t, true, false, (iterationCount ? 5 : 1),
                                          minimum_step_tolerance, 1.);  //GaussNewton in fwd & bwd Sweep
            }
            for (t = prob_->getT() - 2; t > 0; t--)
            {
                updateTimeStep(t, false, true, (iterationCount ? 5 : 0), minimum_step_tolerance, false, 1.);
            }
            break;
        default:
            throw_named("non-existing Sweep mode");
    }
    b_step = 0.0;
    for (t = 0; t < b.size(); t++)
    {
        b_step = std::max((b_old[t] - b[t]).array().abs().maxCoeff(), b_step);
    }
    dampingReference = b;
    // q is set inside of evaluateTrajectory() function
    cost = evaluateTrajectory(b);
    if (debug_) HIGHLIGHT("Iteration: " << iterationCount << ", Sweep: " << sweep << ", updates: " << updateCount << ", cost(ctrl/task/total): " << costControl.sum() << "/" << costTask.sum() << "/" << cost << " (dq=" << b_step << ", damping=" << damping << ")");
    if (cost < 0) return -1.0;
    bestSweep = sweep;

    // If damping (similar to line-search) is being used, consider reverting this step
    if (damping) perhapsUndoStep();

    sweep++;
    if (sweepImprovedCost)
    {
        // HIGHLIGHT("Sweep improved cost, increasing iteration count and resetting sweep count");
        iterationCount++;
        sweep = 0;
        prob_->setCostEvolution(iterationCount, cost);
    }

    return b_step;
}

void AICOsolver::rememberOldState()
{
    s_old = s;
    Sinv_old = Sinv;
    v_old = v;
    Vinv_old = Vinv;
    r_old = r;
    R_old = R;
    Binv_old = Binv;
    rhat_old = rhat;
    b_old = b;
    r_old = r;
    q_old = q;
    qhat_old = qhat;
    cost_old = cost;
    costControl_old = costControl;
    costTask_old = costTask;
    bestSweep_old = bestSweep;
    b_step_old = b_step;
}

void AICOsolver::perhapsUndoStep()
{
    if (cost > cost_old)
    {
        sweepImprovedCost = false;
        damping *= 10.;
        s = s_old;
        Sinv = Sinv_old;
        v = v_old;
        Vinv = Vinv_old;
        r = r_old;
        R = R_old;
        Binv = Binv_old;
        rhat = rhat_old;
        b = b_old;
        r = r_old;
        q = q_old;
        qhat = qhat_old;
        cost = cost_old;
        dampingReference = b_old;
        costControl = costControl_old;
        costTask = costTask_old;
        bestSweep = bestSweep_old;
        b_step = b_step_old;
        if (debug_) HIGHLIGHT("Reverting to previous line-search step (" << bestSweep << ")");
    }
    else
    {
        sweepImprovedCost = true;
        damping /= 5.;
    }
}
} /* namespace exotica */
