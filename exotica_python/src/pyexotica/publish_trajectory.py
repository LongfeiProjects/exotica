from time import sleep
import matplotlib.pyplot as plt
import signal


def sigIntHandler(signal, frame):
    raise KeyboardInterrupt


def publishPose(q, problem, t=0.0):
    problem.getScene().Update(q, t)
    problem.getScene().getSolver().publishFrames()


def publishTrajectory(traj, T, problem):
    signal.signal(signal.SIGINT, sigIntHandler)
    print('Playing back trajectory '+str(T)+'s')
    dt = float(T)/float(len(traj))
    t = 0
    while True:
        try:
            publishPose(traj[t], problem, float(t)*dt)
            sleep(dt)
            t = (t+1) % len(traj)
        except KeyboardInterrupt:
            break


def publishTimeIndexedTrajectory(traj, Ts, problem, once=False):
    signal.signal(signal.SIGINT, sigIntHandler)
    print('Playing back trajectory '+str(len(Ts)) +
          ' states in '+str(Ts[len(Ts)-1]))
    idx = 0
    while True:
        try:
            for i in range(1, len(Ts)):
                publishPose(traj[i], problem, Ts[i])
                sleep(Ts[i]-Ts[i-1])
            if once:
                break
        except KeyboardInterrupt:
            break


def plot(solution):
    print('Plotting the solution')
    plt.plot(solution, '.-')
    plt.show()
