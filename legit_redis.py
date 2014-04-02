import time
import datetime
import subprocess
tmpfilename = "tmp.legit"
logfile = open("legit.log", 'a')
while True:
    with open(tmpfilename, 'w') as f:
        subprocess.call("git pull origin master".split(), stdout=f, stderr=f)


    with open(tmpfilename, 'rU') as f:
        content = f.read()
        if "already up-to-date" not in content.lower():
            subprocess.call("killall level1".split())
            logfile.write("[%s] Resetting...\n" %datetime.datetime.now())
            subprocess.call("git reset --hard origin/master".split())
            subprocess.Popen("nohup ./level1 &".split())
            logfile.write("[%s] Updated repo\n" %datetime.datetime.now())
            logfile.flush()

        else:
            logfile.write("[%s] all okay\n" %datetime.datetime.now())
            logfile.flush()

    time.sleep(1) #danger:legit code



