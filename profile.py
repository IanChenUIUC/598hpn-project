import geni.portal as portal
import geni.rspec.pg as pg

INPUTS = "urn:publicid:IDN+emulab.net:ece509+stdataset+test-dataset"
RESULTS = "urn:publicid:IDN+emulab.net:ece509+stdataset+test-results"
WRITERS = {"populate-friendster"}


def mount_dataset(request, node, name, urn, path, readonly):
    iface = node.addInterface()
    store = request.RemoteBlockstore(name, path)
    store.dataset = urn
    if readonly:
        store.readonly = True

    link = request.Link("%s-link" % name)
    link.addInterface(iface)
    link.addInterface(store.interface)
    link.best_effort = True
    link.vlan_tagging = True


pc = portal.context
pc.defineParameter("image", "Disk image", portal.ParameterType.IMAGE,
                   "urn:publicid:IDN+emulab.net+image+emulab-ops:UBUNTU24-64-STD")
pc.defineParameter("hardware", "Hardware type (empty for any)", portal.ParameterType.NODETYPE, "")
pc.defineParameter("task", "Task to run at boot (none for interactive)",
                   portal.ParameterType.STRING, "none")
pc.defineParameter("terminate", "Terminate the experiment when the task finishes",
                   portal.ParameterType.BOOLEAN, False)
params = pc.bindParameters()

request = pc.makeRequestRSpec()

node = request.RawPC("node")
node.disk_image = params.image
if params.hardware:
    node.hardware_type = params.hardware

mount_dataset(request, node, "inputs", INPUTS, "/data", params.task not in WRITERS)
mount_dataset(request, node, "results", RESULTS, "/results", False)

if params.task != "none":
    command = "sudo /local/repository/scripts/boot.sh %s" % params.task
    if params.terminate:
        command += "; python3 /local/repository/scripts/terminate-self.py"
    node.addService(pg.Execute(shell="bash", command=command))

pc.printRequestRSpec(request)
