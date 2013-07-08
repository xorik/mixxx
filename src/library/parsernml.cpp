#include "parsernml.h"

#include <QtXml/qdom.h>
#include <QTextStream>


ParserNml::ParserNml()
{
}

ParserNml::~ParserNml()
{
}


bool ParserNml::writeNMLFile(const QString &file_str,
        BaseSqlTableModel* pPlaylistTableModel)
{
    // NML header
    QDomDocument doc;
    QDomProcessingInstruction instr = doc.createProcessingInstruction(
            "xml", "version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"");
    doc.appendChild(instr);

    QDomElement nml = doc.createElement("NML");
    nml.setAttribute("VERSION", "14");
    doc.appendChild(nml);

    QDomElement head = doc.createElement("HEAD");
    head.setAttribute("COMPANY", "http://mixxx.org/");
    head.setAttribute("PROGRAM", "Mixxx");
    nml.appendChild(head);

    // Playlist entries
    int rows = pPlaylistTableModel->rowCount();
    QDomElement collection = doc.createElement("COLLECTION");
    collection.setAttribute("ENTRIES", rows);
    nml.appendChild(collection);

    QDomElement playlists = doc.createElement("PLAYLISTS");
    QDomElement playlists_node = doc.createElement("NODE");
    playlists_node.setAttribute("TYPE", "FOLDER");
    playlists_node.setAttribute("NAME", "$ROOT");
    QDomElement playlists_subnodes = doc.createElement("SUBNODES");
    playlists_subnodes.setAttribute("COUNT", "1");
    QDomElement playlists_node2 = doc.createElement("NODE");
    playlists_node2.setAttribute("TYPE", "PLAYLIST");
    // TODO: get playlist's name
    playlists_node2.setAttribute("NAME", "test");
    QDomElement playlist = doc.createElement("PLAYLIST");
    playlist.setAttribute("ENTRIES", rows);
    playlist.setAttribute("TYPE", "LIST");

    nml.appendChild(playlists);
    playlists.appendChild(playlists_node);
    playlists_node.appendChild(playlists_subnodes);
    playlists_subnodes.appendChild(playlists_node2);
    playlists_node2.appendChild(playlist);

    // Export tracks
    for (int i = 0; i < rows; i++) {
        QDomElement entry = doc.createElement("ENTRY");
        QDomElement location = doc.createElement("LOCATION");
        QDomElement entry2 = doc.createElement("ENTRY");
        QDomElement primarykey = doc.createElement("PRIMARYKEY");
        QDomElement album = doc.createElement("ALBUM");
        QDomElement info = doc.createElement("INFO");
        QDomElement tempo = doc.createElement("TEMPO");
        QDomElement cue;

        QModelIndex index = pPlaylistTableModel->index(i,0);
        TrackPointer pTrack = pPlaylistTableModel->getTrack(index);

        entry.setAttribute("TITLE", pTrack->getTitle());
        entry.setAttribute("ARTIST", pTrack->getArtist());

        location.setAttribute("DIR", "/:");
        location.setAttribute("FILE", pTrack->getFilename());
        location.setAttribute("VOLUME", "");
        location.setAttribute("VOLUMEID", "");

        album.setAttribute("TRACK", pTrack->getTrackNumber());
        album.setAttribute("TITLE", pTrack->getAlbum());

        info.setAttribute("BITRATE", pTrack->getBitrateStr());
        info.setAttribute("GENRE", pTrack->getGenre());
        info.setAttribute("COMMENT", pTrack->getComment());
        info.setAttribute("KEY", pTrack->getKey());

        // BPM
        tempo.setAttribute("BPM_TRANSIENTCOHERENCE", pTrack->getBpm());
        tempo.setAttribute("BPM", pTrack->getBpm());
        tempo.setAttribute("BPM_QUALITY", "100");

        entry.appendChild(location);
        entry.appendChild(album);
        entry.appendChild(info);
        entry.appendChild(tempo);

        // Hotcues
        const QList<Cue*>& cuePoints = pTrack->getCuePoints();
        QListIterator<Cue*> it(cuePoints);
        while (it.hasNext()) {
            Cue* pCue = it.next();

            if (pCue->getType() != Cue::CUE && pCue->getType() != Cue::LOAD)
                continue;

            int hotcue = pCue->getHotCue();
            if (hotcue != -1) {
                cue = doc.createElement("CUE_V2");
                cue.setAttribute("NAME", pCue->getLabel());
                cue.setAttribute("DISPL_ORDER", "0");
                cue.setAttribute("TYPE", "0");
                cue.setAttribute("LEN", "0");
                cue.setAttribute("REPEATS", "-1");
                cue.setAttribute("HOTCUE", hotcue);
                // 500 is got by experiment
                cue.setAttribute("START", (float)pCue->getPosition() /
                        pTrack->getSampleRate() * 500);

                entry.appendChild(cue);
            }

            // Copy files
            QDir d = QFileInfo(file_str).absoluteDir();
            QFile::copy( pTrack->getLocation(), d.absolutePath() + "/" +
                    pTrack->getFilename() );
        }

        collection.appendChild(entry);

        primarykey.setAttribute("TYPE", "TRACK");
        primarykey.setAttribute("KEY", QString("/:")+pTrack->getFilename() );

        entry2.appendChild(primarykey);
        playlist.appendChild(entry2);
    }

    // Save playlist
    QFile file(file_str);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(NULL,tr("Playlist Export Failed"),
                             tr("Could not create file") + " " + file_str);
        return false;
    }

    QTextStream out(&file);
    doc.save(out, 4);

    return true;
}
