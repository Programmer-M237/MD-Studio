#pragma once
// =============================================================================
//  ZipWriter.h - tiny ZIP archive writer (enough for .docx files).
//  Uses Qt's built-in zlib through qCompress() to get raw DEFLATE data.
// =============================================================================
#include <QByteArray>
#include <QList>
#include <QString>

class ZipWriter {
public:
    void add(const QString &name, const QByteArray &data)
    {
        Entry e;
        e.name = name.toUtf8();
        e.crc  = crc32(data);
        e.usize = quint32(data.size());

        // qCompress => 4 byte length | 2 byte zlib header | deflate | 4 byte adler32.
        // Stripping header+trailer leaves the raw DEFLATE stream a ZIP expects.
        QByteArray z = qCompress(data, 9);
        if (z.size() > 10 && z.size() - 10 < data.size()) {
            e.method = 8;
            e.payload = z.mid(6, z.size() - 10);
        } else {
            e.method = 0;
            e.payload = data;
        }
        e.csize  = quint32(e.payload.size());
        e.offset = quint32(m_out.size());

        le32(m_out, 0x04034b50);            // local file header signature
        le16(m_out, 20);                    // version needed
        le16(m_out, 0x0800);                // flags: UTF-8 file names
        le16(m_out, e.method);
        le16(m_out, kDosTime);
        le16(m_out, kDosDate);
        le32(m_out, e.crc);
        le32(m_out, e.csize);
        le32(m_out, e.usize);
        le16(m_out, quint16(e.name.size()));
        le16(m_out, 0);                     // extra length
        m_out += e.name;
        m_out += e.payload;
        m_entries.append(e);
    }

    QByteArray finish()
    {
        const quint32 cdStart = quint32(m_out.size());
        for (const Entry &e : m_entries) {
            le32(m_out, 0x02014b50);        // central directory signature
            le16(m_out, 20); le16(m_out, 20);
            le16(m_out, 0x0800);
            le16(m_out, e.method);
            le16(m_out, kDosTime); le16(m_out, kDosDate);
            le32(m_out, e.crc); le32(m_out, e.csize); le32(m_out, e.usize);
            le16(m_out, quint16(e.name.size()));
            le16(m_out, 0); le16(m_out, 0); // extra, comment
            le16(m_out, 0); le16(m_out, 0); // disk start, internal attrs
            le32(m_out, 0);                 // external attrs
            le32(m_out, e.offset);
            m_out += e.name;
        }
        const quint32 cdSize = quint32(m_out.size()) - cdStart;
        le32(m_out, 0x06054b50);            // end of central directory
        le16(m_out, 0); le16(m_out, 0);
        le16(m_out, quint16(m_entries.size())); le16(m_out, quint16(m_entries.size()));
        le32(m_out, cdSize); le32(m_out, cdStart);
        le16(m_out, 0);
        return m_out;
    }

private:
    struct Entry { QByteArray name, payload; quint32 crc = 0, csize = 0, usize = 0, offset = 0; quint16 method = 0; };
    QByteArray   m_out;
    QList<Entry> m_entries;
    static constexpr quint16 kDosTime = 0;
    static constexpr quint16 kDosDate = ((2025 - 1980) << 9) | (1 << 5) | 1;

    static void le16(QByteArray &b, quint16 v) { b.append(char(v & 0xff)); b.append(char(v >> 8)); }
    static void le32(QByteArray &b, quint32 v) { le16(b, quint16(v & 0xffff)); le16(b, quint16(v >> 16)); }

    static quint32 crc32(const QByteArray &d)
    {
        static quint32 table[256];
        static bool init = false;
        if (!init) {
            for (quint32 i = 0; i < 256; ++i) {
                quint32 c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            init = true;
        }
        quint32 c = 0xFFFFFFFFu;
        for (uchar b : d) c = table[(c ^ b) & 0xff] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
};
