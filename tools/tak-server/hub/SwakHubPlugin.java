// A test plugin for the TAK federation hub. The SWAK probe observes both roles:
//   interceptor: renames events whose UID ends with -PLUGIN to the callsign SWAK HUB PLUGIN.
//   sender: sends the event SWAK-HUB-PLUGIN-SENDER every two seconds.
//   receiver: writes the UID of each received SWAK test event to a file, at most 10000 lines.
// start.sh runs it in source-file mode with the plugin SDK and the hub libraries.

import com.atakmap.Tak.BinaryBlob;
import com.atakmap.Tak.FederateGroups;
import com.atakmap.Tak.FederatedEvent;
import com.atakmap.Tak.GeoEvent;
import com.atakmap.Tak.ROL;
import java.io.FileWriter;
import java.io.IOException;
import java.io.Writer;
import java.util.concurrent.CompletableFuture;
import tak.server.federation.hub.plugin.FederationHubPluginMetadata;
import tak.server.federation.hub.plugins.PluginInterceptor;
import tak.server.federation.hub.plugins.PluginReceiver;
import tak.server.federation.hub.plugins.PluginSender;
import tak.server.federation.hub.plugins.clients.PluginInterceptorClient;
import tak.server.federation.hub.plugins.clients.PluginReceiverClient;
import tak.server.federation.hub.plugins.clients.PluginSenderClient;

public class SwakHubPlugin {
    static final String CALLSIGN = "SWAK HUB PLUGIN";
    static final String SENDER_UID = "SWAK-HUB-PLUGIN-SENDER";

    static FederationHubPluginMetadata metadata(String name) {
        return FederationHubPluginMetadata.create(name, "SWAK test plugin", "SWAK", "1");
    }

    // PluginBase sets the plugin type on the metadata, so each client keeps one object.
    static class Interceptor implements PluginInterceptorClient {
        private final FederationHubPluginMetadata metadata = metadata("swak-interceptor");

        public FederationHubPluginMetadata getMetadata() {
            return metadata;
        }

        public CompletableFuture<FederatedEvent> interceptFederatedEvent(FederatedEvent message) {
            if (!message.hasEvent() || !message.getEvent().getUid().endsWith("-PLUGIN")) {
                return CompletableFuture.completedFuture(message);
            }
            GeoEvent event = message.getEvent();
            String detail = event.getOther().replaceAll("callsign=\"[^\"]*\"", "callsign=\"" + CALLSIGN + "\"");
            GeoEvent renamed = event.toBuilder().setScreenName(CALLSIGN).setOther(detail).build();
            return CompletableFuture.completedFuture(message.toBuilder().setEvent(renamed).build());
        }

        public CompletableFuture<FederateGroups> interceptFederateGroups(FederateGroups message) {
            return CompletableFuture.completedFuture(message);
        }

        public CompletableFuture<BinaryBlob> interceptBinaryBlob(BinaryBlob message) {
            return CompletableFuture.completedFuture(message);
        }

        public CompletableFuture<ROL> interceptROL(ROL message) {
            return CompletableFuture.completedFuture(message);
        }
    }

    static class Sender implements PluginSenderClient {
        private final FederationHubPluginMetadata metadata = metadata("swak-sender");

        public FederationHubPluginMetadata getMetadata() {
            return metadata;
        }
    }

    static class Receiver implements PluginReceiverClient {
        private static final int MaxLines = 10000;
        private final FederationHubPluginMetadata metadata = metadata("swak-receiver");
        private final Writer writer;
        private int lines = 0;

        Receiver(String path) throws IOException {
            writer = new FileWriter(path, false);
        }

        public FederationHubPluginMetadata getMetadata() {
            return metadata;
        }

        public synchronized void receiveFederatedEvent(FederatedEvent message) {
            String uid = message.hasEvent() ? message.getEvent().getUid() : "";
            if (!uid.startsWith("SWAK-COMPAT-") || lines >= MaxLines) {
                return;
            }
            try {
                writer.write(uid + "\n");
                writer.flush();
                ++lines;
            } catch (IOException e) {
                System.err.println("Cannot write the receiver file: " + e.getMessage());
            }
        }

        public void receiveFederateGroups(FederateGroups message) {}

        public void receiveBinaryBlob(BinaryBlob message) {}

        public void receiveROL(ROL message) {}
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && args[0].equals("interceptor")) {
            new PluginInterceptor(new Interceptor());
            Thread.currentThread().join();
        } else if (args.length == 2 && args[0].equals("receiver")) {
            new PluginReceiver(new Receiver(args[1]));
            Thread.currentThread().join();
        } else if (args.length == 2 && args[0].equals("sender")) {
            PluginSender sender = new PluginSender(new Sender());
            while (true) {
                long now = System.currentTimeMillis();
                GeoEvent event = GeoEvent.newBuilder()
                        .setUid(SENDER_UID)
                        .setType("a-f-G-U-C")
                        .setCoordSource("m-g")
                        .setSendTime(now)
                        .setStartTime(now)
                        .setStaleTime(now + 120000)
                        .setLat(0.75)
                        .setLon(0.75)
                        .setHae(10)
                        .setCe(9999999)
                        .setLe(9999999)
                        .setScreenName("SWAK HUB SENDER")
                        .setOther("<contact callsign=\"SWAK HUB SENDER\"/>")
                        .build();
                sender.sendMessage(FederatedEvent.newBuilder().setEvent(event).addFederateGroups(args[1]).build());
                Thread.sleep(2000);
            }
        } else {
            System.err.println("Usage: SwakHubPlugin interceptor | sender <group> | receiver <file>");
            System.exit(2);
        }
    }
}
