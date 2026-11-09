import geni.portal as portal
import geni.rspec.pg as pg

INPUTS = "urn:publicid:IDN+utah.cloudlab.us:ece509-pg0+stdataset+test-dataset"
RESULTS = "urn:publicid:IDN+utah.cloudlab.us:ece509-pg0+stdataset+test-results"
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
                   "urn:publicid:IDN+emulab.net+image+ECE509:experiments",
                   legalValues=["urn:publicid:IDN+emulab.net+image+ECE509:experiments",
                                "urn:publicid:IDN+emulab.net+image+emulab-ops:UBUNTU24-64-STD"])
pc.defineParameter("hardware", "Hardware type (empty for any)", portal.ParameterType.NODETYPE, "")
pc.defineParameter("nodes", "Number of nodes", portal.ParameterType.INTEGER, 1)
pc.defineParameter("bandwidth", "LAN bandwidth in Gb/s (with more than one node)",
                   portal.ParameterType.INTEGER, 100)
pc.defineParameter("datasets", "Mount the inputs and results datasets on node0",
                   portal.ParameterType.BOOLEAN, True)
pc.defineParameter("task", "Task to run at boot (none for interactive)",
                   portal.ParameterType.STRING, "none",
                   legalValues=["none", "populate-friendster", "baseline", "galois-cc"])
pc.defineParameter("terminate", "Terminate the experiment when the task finishes",
                   portal.ParameterType.BOOLEAN, False)
params = pc.bindParameters()

request = pc.makeRequestRSpec()

nodes = []
for i in range(params.nodes):
    node = request.RawPC("node%d" % i)
    node.disk_image = params.image
    if params.hardware:
        node.hardware_type = params.hardware
    nodes.append(node)

if params.nodes > 1:
    lan = request.LAN("lan")
    lan.bandwidth = params.bandwidth * 1000000  # kbps
    for i, node in enumerate(nodes):
        iface = node.addInterface("if%d" % i)
        iface.addAddress(pg.IPv4Address("192.168.1.%d" % (i + 1), "255.255.255.0"))
        lan.addInterface(iface)

if params.datasets:
    mount_dataset(request, nodes[0], "inputs", INPUTS, "/data", params.task not in WRITERS)
    mount_dataset(request, nodes[0], "results", RESULTS, "/results", False)

if params.task != "none":
    command = "sudo /local/repository/scripts/boot.sh %s" % params.task
    if params.terminate:
        command += "; python3 /local/repository/scripts/terminate-self.py"
    nodes[0].addService(pg.Execute(shell="bash", command=command))

pc.printRequestRSpec(request)
