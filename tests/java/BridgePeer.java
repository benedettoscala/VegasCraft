import dev.vegascraft.TelemetryBridge;

/** Exercises the production Java bridge in a real 64-bit JVM against the native 32-bit host. */
public final class BridgePeer {
    public static void main(String[] args) throws Exception {
        String name = args.length > 0 ? args[0] : TelemetryBridge.DEFAULT_NAME;
        boolean expectReject = args.length > 1 && args[1].equals("reject");
        try (var bridge = new TelemetryBridge(name)) {
            long deadline = System.currentTimeMillis() + 5000;
            boolean connected = false;
            while (System.currentTimeMillis() < deadline && !(connected = bridge.connect())) {
                if (expectReject && !bridge.error().isEmpty()) break;
                Thread.sleep(20);
            }
            if (expectReject) {
                if (connected || bridge.error().isEmpty()) throw new AssertionError("Bad protocol or size accepted");
                System.out.println("REJECTED " + bridge.error()); return;
            }
            if (!connected) throw new AssertionError("Cannot connect: " + bridge.error());
            var state = bridge.readHost();
            if (state == null || !state.inGame() || state.worldId() != 0x3C || state.x() != 12.5 || state.y() != 64 || state.z() != -7.25)
                throw new AssertionError("Native state mismatch: " + state);
            while (bridge.alive() && System.currentTimeMillis() < deadline) {
                bridge.heartbeat();
                bridge.publish(1, 21.25, 80.5, -9.75, 90, 0, 42);
                Thread.sleep(10);
            }
            if (bridge.alive()) throw new AssertionError("Host did not disconnect");
            System.out.println("PASS x64 Java received x86 state, sent MC state, detected host shutdown");
        }
    }
}
