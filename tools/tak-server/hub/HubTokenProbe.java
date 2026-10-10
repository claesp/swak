// Checks the token port of the TAK federation hub with its own gRPC client classes. It prints
// one JSON object with a passed flag and a detail text for each check.
//   hubTokenForged: the hub refuses a token that it did not sign.
//   hubTokenNotAllowed: the hub issues no token for a CA group without allowTokenAuth.
//   hubTokenIssued: the hub issues a token for a CA group with allowTokenAuth and accepts it.
//   hubTokenTampered: the hub refuses the issued token after one changed signature character.
//   hubTokenWithoutKey: the hub issues the token from the certificate alone. This records the
//     fact that the token port asks for no proof of the private key.
//   hubManualToken: the hub accepts a token from the hub UI that the policy lists.
//   hubManualTokenUnlisted: the hub refuses a valid token from the hub UI that the policy does
//     not list.
// Usage: HubTokenProbe <ca.pem> <allowed.pem> <refused.pem> <port> [tokens.json]

import com.atakmap.Tak.BinaryBlob;
import com.atakmap.Tak.ClientHealth;
import com.atakmap.Tak.FederatedChannelGrpc;
import com.atakmap.Tak.ServerHealth;
import com.google.protobuf.ByteString;
import io.grpc.ManagedChannel;
import io.grpc.Status;
import io.grpc.StatusRuntimeException;
import io.grpc.netty.GrpcSslContexts;
import io.grpc.netty.NettyChannelBuilder;
import io.grpc.stub.StreamObserver;
import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import tak.server.federation.TokenAuthCredential;

public class HubTokenProbe {
    static final Map<String, String> results = new LinkedHashMap<>();

    static void record(String name, boolean passed, String detail) {
        String text = detail == null ? "" : detail.replace("\\", "\\\\").replace("\"", "\\\"");
        if (text.length() > 200) {
            text = text.substring(0, 200);
        }
        results.put(name, "{\"passed\":" + passed + ",\"detail\":\"" + text + "\"}");
    }

    static BinaryBlob certificate(String path) throws Exception {
        try (FileInputStream input = new FileInputStream(path)) {
            X509Certificate cert = (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(input);
            return BinaryBlob.newBuilder().setData(ByteString.copyFrom(cert.getEncoded())).build();
        }
    }

    static FederatedChannelGrpc.FederatedChannelBlockingStub stub(ManagedChannel channel) {
        return FederatedChannelGrpc.newBlockingStub(channel).withDeadlineAfter(10, TimeUnit.SECONDS);
    }

    // Returns the first answer of a health check with the token: the health status that the
    // hub sends, or the error code. A health status shows that the hub accepted the token.
    // Without an event stream, the hub sends NOT_CONNECTED and keeps the call open.
    static String health(ManagedChannel channel, String token) throws Exception {
        CompletableFuture<String> answer = new CompletableFuture<>();
        FederatedChannelGrpc.newStub(channel)
                .withDeadlineAfter(10, TimeUnit.SECONDS)
                .withCallCredentials(new TokenAuthCredential(token))
                .healthCheck(ClientHealth.newBuilder().setStatus(ClientHealth.ServingStatus.SERVING).build(),
                        new StreamObserver<ServerHealth>() {
                            public void onNext(ServerHealth value) {
                                answer.complete(value.getStatus().name());
                            }

                            public void onError(Throwable error) {
                                answer.complete(Status.fromThrowable(error).getCode().name());
                            }

                            public void onCompleted() {
                                answer.complete("COMPLETED");
                            }
                        });
        return answer.get(15, TimeUnit.SECONDS);
    }

    // Reads a string field from the flat JSON object of tools/tak_server.py. JWT text has no quotes.
    static String field(String json, String name) {
        Matcher match = Pattern.compile("\"" + name + "\"\\s*:\\s*\"([^\"]*)\"").matcher(json);
        return match.find() ? match.group(1) : "";
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 4 && args.length != 5) {
            System.err.println("Usage: HubTokenProbe <ca.pem> <allowed.pem> <refused.pem> <port> [tokens.json]");
            System.exit(2);
        }
        ManagedChannel channel = NettyChannelBuilder.forAddress("localhost", Integer.parseInt(args[3]))
                .sslContext(GrpcSslContexts.forClient().trustManager(new File(args[0])).build())
                .build();
        try {
            String forged = health(channel, "eyJhbGciOiJIUzI1NiJ9.eyJjbGllbnRHcm91cHMiOltdfQ.c3dhaw");
            record("hubTokenForged", forged.equals("UNAUTHENTICATED"), "health check: " + forged);

            try {
                String token = stub(channel).getAuthTokenByX509(certificate(args[2])).getToken();
                record("hubTokenNotAllowed", false, "issued a token of " + token.length() + " characters");
            } catch (StatusRuntimeException e) {
                record("hubTokenNotAllowed", e.getStatus().getCode() == Status.Code.UNAUTHENTICATED,
                        e.getStatus().getCode() + ": " + e.getStatus().getDescription());
            }

            String token = "";
            try {
                token = stub(channel).getAuthTokenByX509(certificate(args[1])).getToken();
            } catch (StatusRuntimeException e) {
                record("hubTokenIssued", false, e.getStatus().getCode() + ": " + e.getStatus().getDescription());
            }
            if (!token.isEmpty()) {
                String accepted = health(channel, token);
                record("hubTokenIssued", accepted.equals("NOT_CONNECTED") || accepted.equals("SERVING"),
                        "token of " + token.length() + " characters, health check: " + accepted);
                // The last base64url character can hold unused bits, so change the first
                // character of the signature.
                int signature = token.lastIndexOf('.') + 1;
                char first = token.charAt(signature);
                String tampered = token.substring(0, signature) + (first == 'A' ? 'B' : 'A')
                        + token.substring(signature + 1);
                String refused = health(channel, tampered);
                record("hubTokenTampered", refused.equals("UNAUTHENTICATED"), "health check: " + refused);
                // This client posted only the public certificate. It never used the private key.
                record("hubTokenWithoutKey", true,
                        "the hub issued a token for a posted certificate without proof of its key");
            }
            if (args.length == 5) {
                // tools/tak_server.py writes the two UI tokens as {"listed": ..., "unlisted": ...}.
                String tokens = new String(Files.readAllBytes(Paths.get(args[4])), StandardCharsets.UTF_8);
                String listed = field(tokens, "listed");
                String unlisted = field(tokens, "unlisted");
                String accepted = health(channel, listed);
                record("hubManualToken", accepted.equals("NOT_CONNECTED") || accepted.equals("SERVING"),
                        "health check: " + accepted);
                String refused = health(channel, unlisted);
                record("hubManualTokenUnlisted", refused.equals("UNAUTHENTICATED"), "health check: " + refused);
            }
        } finally {
            channel.shutdownNow();
        }
        StringBuilder json = new StringBuilder("{");
        for (Map.Entry<String, String> entry : results.entrySet()) {
            if (json.length() > 1) {
                json.append(',');
            }
            json.append('"').append(entry.getKey()).append("\":").append(entry.getValue());
        }
        System.out.println(json.append('}'));
    }
}
