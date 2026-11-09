package io.github.lessunderrated.bo2;

import android.content.ContentResolver;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.file.Files;
import java.util.Locale;

/**
 * The app's internal storage (logs/, the game's saves and config once it writes them) as a root of
 * the system's Files app, so logs can be read and saves backed up without a computer. Taken from
 * LessUnderRated's Chronicle-rs ChronicleDocumentsProvider, itself adapted from doukutsu-rs's
 * DoukutsuDocumentsProvider (MIT; THIRD_PARTY_NOTICES.md). A document's id is its canonical path, and
 * only paths inside the app's files directory are served. The imported game data lives in the
 * external files directory instead (SetupActivity) and is not served here.
 */
public class Bo2DocumentsProvider extends DocumentsProvider {
    private static final String[] ROOT_PROJECTION = {
        Root.COLUMN_DOCUMENT_ID, Root.COLUMN_ROOT_ID, Root.COLUMN_ICON, Root.COLUMN_TITLE,
        Root.COLUMN_MIME_TYPES, Root.COLUMN_AVAILABLE_BYTES, Root.COLUMN_FLAGS,
    };
    private static final String[] DOCUMENT_PROJECTION = {
        Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_SIZE,
        Document.COLUMN_LAST_MODIFIED, Document.COLUMN_MIME_TYPE, Document.COLUMN_FLAGS,
    };

    private static String canonical(File file) {
        try {
            return file.getCanonicalPath();
        } catch (IOException e) {
            return file.getAbsolutePath();
        }
    }

    private String rootId() {
        return canonical(getContext().getFilesDir());
    }

    private File fileFor(String documentId) throws FileNotFoundException {
        File file = new File(documentId);
        String path = canonical(file);
        String root = rootId();
        if (!path.equals(root) && !path.startsWith(root + File.separator)) {
            throw new FileNotFoundException(documentId + " is outside the app's files");
        }
        return file;
    }

    private void notifyParent(File file) {
        Uri uri = DocumentsContract.buildChildDocumentsUri(BuildConfig.DOCUMENTS_AUTHORITY, canonical(file.getParentFile()));
        getContext().getContentResolver().notifyChange(uri, null, ContentResolver.NOTIFY_UPDATE);
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor result = new MatrixCursor(projection != null ? projection : ROOT_PROJECTION);
        File files = getContext().getFilesDir();
        MatrixCursor.RowBuilder row = result.newRow();
        row.add(Root.COLUMN_DOCUMENT_ID, rootId());
        row.add(Root.COLUMN_ROOT_ID, rootId());
        row.add(Root.COLUMN_ICON, R.drawable.ic_launcher);
        row.add(Root.COLUMN_TITLE, getContext().getString(R.string.app_name));
        row.add(Root.COLUMN_MIME_TYPES, "*/*");
        row.add(Root.COLUMN_AVAILABLE_BYTES, files.getFreeSpace());
        row.add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_IS_CHILD | Root.FLAG_SUPPORTS_CREATE | Root.FLAG_LOCAL_ONLY);
        return result;
    }

    @Override
    public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        MatrixCursor result = new MatrixCursor(projection != null ? projection : DOCUMENT_PROJECTION);
        addRow(result, fileFor(documentId));
        return result;
    }

    @Override
    public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder)
            throws FileNotFoundException {
        MatrixCursor result = new MatrixCursor(projection != null ? projection : DOCUMENT_PROJECTION);
        File parent = fileFor(parentDocumentId);
        File[] children = parent.listFiles();
        if (children != null) {
            for (File child : children) {
                addRow(result, child);
            }
        }
        result.setNotificationUri(getContext().getContentResolver(),
            DocumentsContract.buildChildDocumentsUri(BuildConfig.DOCUMENTS_AUTHORITY, parentDocumentId));
        return result;
    }

    @Override
    public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal)
            throws FileNotFoundException {
        return ParcelFileDescriptor.open(fileFor(documentId), ParcelFileDescriptor.parseMode(mode));
    }

    @Override
    public String createDocument(String parentDocumentId, String mimeType, String displayName)
            throws FileNotFoundException {
        File parent = fileFor(parentDocumentId);
        File file = new File(parent, displayName);
        for (int n = 1; file.exists(); n++) {
            file = new File(parent, String.format(Locale.US, "%s (%d)", displayName, n));
        }
        try {
            boolean made = Document.MIME_TYPE_DIR.equals(mimeType) ? file.mkdir() : file.createNewFile();
            if (!made) {
                throw new FileNotFoundException("cannot create " + file);
            }
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
        notifyParent(file);
        return canonical(file);
    }

    @Override
    public void deleteDocument(String documentId) throws FileNotFoundException {
        File file = fileFor(documentId);
        if (canonical(file).equals(rootId())) {
            throw new FileNotFoundException("the root cannot be deleted");
        }
        deleteRecursive(file);
        notifyParent(file);
    }

    @Override
    public String renameDocument(String documentId, String displayName) throws FileNotFoundException {
        File file = fileFor(documentId);
        File target = fileFor(canonical(new File(file.getParentFile(), displayName)));
        try {
            Files.move(file.toPath(), target.toPath());
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
        notifyParent(file);
        return canonical(target);
    }

    @Override
    public boolean isChildDocument(String parentDocumentId, String documentId) {
        return documentId.equals(parentDocumentId) || documentId.startsWith(parentDocumentId + File.separator);
    }

    @Override
    public String getDocumentType(String documentId) throws FileNotFoundException {
        File file = fileFor(documentId);
        return file.isDirectory() ? Document.MIME_TYPE_DIR : mimeType(file);
    }

    private static void deleteRecursive(File file) {
        File[] children = file.isDirectory() && !Files.isSymbolicLink(file.toPath()) ? file.listFiles() : null;
        if (children != null) {
            for (File child : children) {
                deleteRecursive(child);
            }
        }
        file.delete();
    }

    private static String mimeType(File file) {
        String extension = MimeTypeMap.getFileExtensionFromUrl(file.getName().toLowerCase(Locale.ROOT));
        String type = extension != null ? MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension) : null;
        return type != null ? type : "application/octet-stream";
    }

    private void addRow(MatrixCursor result, File file) throws FileNotFoundException {
        if (!file.exists()) {
            throw new FileNotFoundException(file + " does not exist");
        }
        int flags = 0;
        String mime;
        if (file.isDirectory()) {
            mime = Document.MIME_TYPE_DIR;
            if (file.canWrite()) {
                flags |= Document.FLAG_DIR_SUPPORTS_CREATE;
            }
        } else {
            mime = mimeType(file);
            if (file.canWrite()) {
                flags |= Document.FLAG_SUPPORTS_WRITE;
            }
        }
        if (!canonical(file).equals(rootId())) {
            flags |= Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME;
        }
        MatrixCursor.RowBuilder row = result.newRow();
        row.add(Document.COLUMN_DOCUMENT_ID, canonical(file));
        row.add(Document.COLUMN_DISPLAY_NAME, file.getName());
        row.add(Document.COLUMN_SIZE, file.length());
        row.add(Document.COLUMN_LAST_MODIFIED, file.lastModified());
        row.add(Document.COLUMN_MIME_TYPE, mime);
        row.add(Document.COLUMN_FLAGS, flags);
    }
}
