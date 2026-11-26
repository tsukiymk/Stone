import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/** Binary mutations for the existing APK signature rejection fixtures. */
public final class ArchiveMutator {
    public static void main(String[] args) throws Exception {
        byte[] bytes = Files.readAllBytes(Path.of(args[1]));
        switch (args[0]) {
            case "tamper" -> {
                byte[] payload = args[3].getBytes(StandardCharsets.US_ASCII);
                int matches = 0;
                for (int at = 0; at <= bytes.length - payload.length; at++) {
                    if (Arrays.equals(bytes, at, at + payload.length, payload, 0, payload.length)) {
                        bytes[at] ^= 1;
                        matches++;
                    }
                }
                if (matches != 1) throw new IllegalStateException("Expected one stored payload, found " + matches);
            }
            case "truncate" -> {
                if (bytes.length < 128) throw new IllegalStateException("Fixture shorter than truncation limit");
                bytes = Arrays.copyOf(bytes, 128);
            }
            default -> throw new IllegalArgumentException("Unknown archive mutation: " + args[0]);
        }
        Files.write(Path.of(args[2]), bytes);
    }
}
