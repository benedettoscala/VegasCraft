package dev.vegascraft.client.icons;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/**
 * Reads a Fallout 3 / New Vegas archive (BSA version 104): its file list, and a file's bytes
 * (zlib-compressed when the archive or the file's own bit says so).
 */
final class Bsa implements AutoCloseable {
	record Entry(String path, long offset, int size, boolean compressed) {
	}

	private static final int FLAG_DIRECTORY_NAMES = 0x1, FLAG_FILE_NAMES = 0x2, FLAG_COMPRESSED = 0x4, FLAG_EMBEDDED_NAMES = 0x100;
	private static final int SIZE_COMPRESSION_TOGGLE = 0x40000000;

	private final RandomAccessFile file;
	private final int flags;
	final List<Entry> entries = new ArrayList<>();

	Bsa(Path path) throws IOException {
		this.file = new RandomAccessFile(path.toFile(), "r");
		ByteBuffer header = read(0, 36);
		if (header.getInt() != 0x00415342 || header.getInt() != 104) {
			throw new IOException("not a version 104 BSA: " + path);
		}
		int folderOffset = header.getInt();
		this.flags = header.getInt();
		int folders = header.getInt();
		int files = header.getInt();
		int folderNamesLength = header.getInt();  // with their NULs, without the length bytes
		int fileNamesLength = header.getInt();
		if ((this.flags & FLAG_DIRECTORY_NAMES) == 0 || (this.flags & FLAG_FILE_NAMES) == 0) {
			throw new IOException("BSA without names: " + path);
		}
		int[] counts = new int[folders];
		ByteBuffer records = read(folderOffset, folders * 16);
		for (int i = 0; i < folders; i++) {
			records.getLong();
			counts[i] = records.getInt();
			records.getInt();
		}
		// Folder blocks: a name (u8 length with its NUL, then the text), then 16-byte file records.
		long at = folderOffset + folders * 16L;
		ByteBuffer blocks = read(at, folderNamesLength + folders + files * 16 + fileNamesLength);
		List<String> folderOfFile = new ArrayList<>(files);
		List<long[]> recordOfFile = new ArrayList<>(files);
		for (int i = 0; i < folders; i++) {
			int len = blocks.get() & 0xFF;
			byte[] name = new byte[len];
			blocks.get(name);
			String folder = new String(name, 0, Math.max(0, len - 1), StandardCharsets.ISO_8859_1);
			for (int j = 0; j < counts[i]; j++) {
				blocks.getLong();
				long size = blocks.getInt() & 0xFFFFFFFFL;
				long offset = blocks.getInt() & 0xFFFFFFFFL;
				folderOfFile.add(folder);
				recordOfFile.add(new long[] { size, offset });
			}
		}
		byte[] names = new byte[fileNamesLength];
		blocks.get(names);
		int start = 0;
		for (int i = 0; i < folderOfFile.size(); i++) {
			int end = start;
			while (end < names.length && names[end] != 0) {
				end++;
			}
			String name = new String(names, start, end - start, StandardCharsets.ISO_8859_1);
			start = end + 1;
			long size = recordOfFile.get(i)[0];
			boolean compressed = ((this.flags & FLAG_COMPRESSED) != 0) ^ ((size & SIZE_COMPRESSION_TOGGLE) != 0);
			this.entries.add(new Entry((folderOfFile.get(i) + "\\" + name).toLowerCase(java.util.Locale.ROOT), recordOfFile.get(i)[1], (int) (size & 0x3FFFFFFF), compressed));
		}
	}

	private ByteBuffer read(long offset, int length) throws IOException {
		byte[] bytes = new byte[length];
		this.file.seek(offset);
		this.file.readFully(bytes);
		return ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
	}

	byte[] bytes(Entry e) throws IOException {
		ByteBuffer data = read(e.offset(), e.size());
		if ((this.flags & FLAG_EMBEDDED_NAMES) != 0) {
			int len = data.get() & 0xFF;
			data.position(1 + len);
		}
		if (!e.compressed()) {
			byte[] out = new byte[data.remaining()];
			data.get(out);
			return out;
		}
		int original = data.getInt();
		Inflater inflater = new Inflater();
		inflater.setInput(data);
		ByteArrayOutputStream out = new ByteArrayOutputStream(Math.max(original, 64));
		byte[] chunk = new byte[65536];
		try {
			while (!inflater.finished()) {
				int n = inflater.inflate(chunk);
				if (n == 0 && (inflater.needsInput() || inflater.needsDictionary())) {
					break;
				}
				out.write(chunk, 0, n);
			}
		} catch (DataFormatException ex) {
			throw new IOException("corrupt file in BSA: " + e.path(), ex);
		} finally {
			inflater.end();
		}
		return out.toByteArray();
	}

	@Override
	public void close() throws IOException {
		this.file.close();
	}
}
