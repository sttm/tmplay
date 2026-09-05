#!/usr/bin/env python3
"""YouTube Music album search for TPlay.

The script keeps the YTMusic-specific matching logic out of the C++ UI.  It
prints a tab-separated, base64-encoded protocol so titles and artist names
cannot corrupt the response. Album cards are fast; their tracks are fetched
and verified as Art Tracks (ATV) only when the user opens a card.
"""

import base64
import sys


def _b64(value):
    if value is None:
        value = ""
    if isinstance(value, bool):
        value = "1" if value else "0"
    return base64.b64encode(str(value).encode("utf-8")).decode("ascii")


def _line(kind, *values):
    print("\t".join((kind, *(_b64(value) for value in values))))


def _normal(value):
    return " ".join(str(value or "").lower().split())


def _artists(track):
    names = []
    for artist in track.get("artists") or []:
        if isinstance(artist, dict) and artist.get("name"):
            names.append(artist["name"])
    return ", ".join(names) or track.get("artist") or "Unknown Artist"


def _artist_names(track):
    names = {
        _normal(artist.get("name"))
        for artist in track.get("artists") or []
        if isinstance(artist, dict) and artist.get("name")
    }
    if not names and track.get("artist"):
        names.add(_normal(track["artist"]))
    return names


def _album_name(track):
    album = track.get("album")
    if isinstance(album, dict):
        return album.get("name") or ""
    return album or ""


def _album_id(track):
    album = track.get("album")
    if isinstance(album, dict):
        return album.get("id") or ""
    return ""


def _duration(track):
    value = track.get("duration_seconds", track.get("durationSeconds", 0))
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def _video_type(track):
    value = track.get("videoType") or ""
    prefix = "MUSIC_VIDEO_TYPE_"
    return value[len(prefix):] if value.startswith(prefix) else value


def _same_track(target, candidate, require_album=True):
    if _video_type(candidate) != "ATV":
        return False
    if _normal(candidate.get("title")) != _normal(target.get("title")):
        return False
    target_explicit = target.get("isExplicit")
    candidate_explicit = candidate.get("isExplicit")
    if (target_explicit is not None and candidate_explicit is not None and
            target_explicit != candidate_explicit):
        return False
    target_duration = _duration(target)
    candidate_duration = _duration(candidate)
    if target_duration and candidate_duration and abs(target_duration - candidate_duration) > 3:
        return False
    if require_album:
        wanted_album = _normal(_album_name(target))
        if wanted_album and _normal(_album_name(candidate)) != wanted_album:
            return False
    wanted_artists = _artist_names(target)
    candidate_artists = _artist_names(candidate)
    if wanted_artists and candidate_artists and not (wanted_artists & candidate_artists):
        return False
    return bool(candidate.get("videoId"))


def _resolve_atv(yt, track, playlist_tracks, index):
    """Return a verified ATV id or an empty string; never fall back to UGC."""
    if _video_type(track) == "ATV" and track.get("videoId"):
        return track["videoId"]

    if index < len(playlist_tracks):
        candidate = playlist_tracks[index]
        if _same_track(track, candidate, require_album=False):
            return candidate["videoId"]

    query = " ".join(value for value in (
        track.get("title"), _artists(track), _album_name(track)
    ) if value)
    try:
        candidates = yt.search(query, filter="songs", limit=25, ignore_spelling=True)
    except Exception:
        return ""
    matches = [candidate for candidate in candidates if _same_track(track, candidate)]
    if not matches:
        return ""

    expected_duration = _duration(track)
    expected_explicit = track.get("isExplicit")
    matches.sort(key=lambda candidate: (
        0 if candidate.get("isExplicit") == expected_explicit else 1,
        abs(_duration(candidate) - expected_duration) if expected_duration else 999999,
        candidate.get("videoId") or "",
    ))
    return matches[0].get("videoId") or ""


def _thumbnail(item):
    thumbnails = item.get("thumbnails") or []
    if thumbnails:
        return thumbnails[-1].get("url") or ""
    return ""


def _search_key(value):
    return "".join(character for character in _normal(value)
                   if character.isalnum())


def _query_artist_title(query):
    for separator in (" — ", " - ", " – "):
        if separator in query:
            artist, title = query.split(separator, 1)
            return _search_key(artist), _search_key(title)
    return "", ""


def search_tracks(query, limit):
    """Return YTMusic song results with official Art Tracks first."""
    try:
        from ytmusicapi import YTMusic
    except ImportError:
        _line("ERROR", "ytmusicapi is not installed")
        return 2

    try:
        requested = max(1, min(int(limit), 50))
    except (TypeError, ValueError):
        requested = 50
    yt = YTMusic()
    atv_tracks = []
    omv_tracks = []
    seen = set()

    def collect_songs(items):
        for track in items or []:
            video_id = track.get("videoId") or ""
            if not video_id or video_id in seen:
                continue
            seen.add(video_id)
            # Only canonical album cuts can provide an exact MPRE browse id
            # for TPlay's Open album action.
            if _video_type(track) == "ATV" and _album_id(track):
                atv_tracks.append(track)
            elif _video_type(track) == "OMV":
                omv_tracks.append(track)

    try:
        # One public YTMusic request is both faster and more reliable than
        # resolving an artist page first. `videoType=...ATV` is YTMusic's own
        # marker for the original artist's high-quality Art Track.
        collect_songs(yt.search(query, filter="songs", limit=50,
                                ignore_spelling=True))
    except Exception as error:
        _line("ERROR", "YouTube Music song search failed: " + str(error))
        return 1

    artist_query, title_query = _query_artist_title(query)
    query_key = _search_key(query)

    def relevance(track, original_index):
        title = _search_key(track.get("title"))
        artist = _search_key(_artists(track))
        combined = artist + title
        score = 0
        if query_key and query_key == combined:
            score += 20000
        elif query_key and query_key in combined:
            score += 5000
        if artist_query:
            score += 8000 if artist == artist_query else (
                2500 if artist_query in artist else 0)
        if title_query:
            score += 10000 if title == title_query else (
                3000 if title_query in title else 0)
        elif query_key and query_key == title:
            score += 9000
        # Given equal title/artist matches, prefer the explicit album cut.
        if track.get("isExplicit"):
            score += 500
        return (-score, original_index)

    atv_tracks = [track for _, track in sorted(
        enumerate(atv_tracks), key=lambda item: relevance(item[1], item[0]))]

    # An Art Track is a canonical album cut. Only fall back to original music
    # videos when YTMusic did not return one ATV at all; mixing the two makes
    # Open album unreliable because videos often do not carry an album id.
    tracks = atv_tracks[:requested] if atv_tracks else omv_tracks[:10]
    if not tracks:
        _line("ERROR", "No YouTube Music song results found")
        return 1

    for track in tracks:
        _line("SONG", track.get("videoId") or "",
              track.get("title") or "Unknown Track", _artists(track),
              _album_name(track), _album_id(track), _duration(track), track.get("isExplicit"),
              track.get("genre") or "", _thumbnail(track),
              track.get("year") or track.get("releaseYear") or "")
    return 0


def search_albums(query):
    try:
        from ytmusicapi import YTMusic
    except ImportError:
        _line("ERROR", "ytmusicapi is not installed. Run: python3 -m pip install ytmusicapi")
        return 2

    yt = YTMusic()
    try:
        results = yt.search(query, filter="albums", limit=20)
    except Exception as error:
        _line("ERROR", "YouTube Music search failed: " + str(error))
        return 1

    query_artist = _normal(query)

    def artist_priority(result):
        """Put the queried artist's solo releases before collaborations.

        YTMusic returns a useful relevance order, but can interleave a primary
        artist's discography with joint releases and tribute/variation artists.
        Keep that original order inside each group by using a stable sort.
        Album-title queries do not match an artist name and remain unchanged.
        """
        names = [_normal(artist.get("name"))
                 for artist in result.get("artists") or []
                 if isinstance(artist, dict) and artist.get("name")]
        if len(names) == 1 and names[0] == query_artist:
            return 0  # solo release by the requested artist
        if query_artist and query_artist in names:
            return 1  # collaboration that includes the requested artist
        if query_artist and any(query_artist in name or name in query_artist
                                for name in names):
            return 2  # artist-name variation
        return 3

    seen = set()
    albums = []
    for original_index, result in enumerate(results):
        browse_id = result.get("browseId")
        if not browse_id or browse_id in seen:
            continue
        seen.add(browse_id)
        albums.append((artist_priority(result), original_index, browse_id,
                       result.get("title") or "Unknown Album",
                       _artists(result), result.get("year") or "",
                       _thumbnail(result)))

    if not albums:
        _line("ERROR", "No official albums found")
        return 1
    for _, _, browse_id, title, artist, year, artwork in sorted(albums):
        _line("ALBUM", browse_id, title, artist, year, artwork)
    return 0


def album_tracks(browse_id):
    try:
        from ytmusicapi import YTMusic
    except ImportError:
        _line("ERROR", "ytmusicapi is not installed. Run: python3 -m pip install ytmusicapi")
        return 2

    yt = YTMusic()
    try:
        album = yt.get_album(browse_id)
    except Exception as error:
        _line("ERROR", "YouTube Music album lookup failed: " + str(error))
        return 1
    tracks = album.get("tracks") or []
    title = album.get("title") or "Unknown Album"
    artist = _artists(album) if album.get("artists") else "Unknown Artist"
    year = album.get("year") or ""
    artwork = _thumbnail(album)
    if not tracks:
        _line("ERROR", "Album has no playable tracks")
        return 1

    playlist_tracks = []
    audio_playlist_id = album.get("audioPlaylistId")
    if audio_playlist_id:
        try:
            playlist_tracks = (yt.get_playlist(audio_playlist_id, limit=None).get("tracks") or [])
        except Exception:
            pass

    album_tracks = []
    for index, item in enumerate(tracks):
        track = dict(item)
        track["album"] = {"name": title}
        video_id = _resolve_atv(yt, track, playlist_tracks, index)
        if video_id:
            album_tracks.append((index + 1, track, video_id))
    if not album_tracks:
        _line("ERROR", "No verified YouTube Music Art Tracks in this album")
        return 1

    _line("ALBUM", browse_id, title, artist, year, artwork)
    for index, track, video_id in album_tracks:
        _line("TRACK", browse_id, index, track.get("title") or "Unknown Track",
              _artists(track), title, _duration(track), track.get("isExplicit"),
              track.get("genre") or "", _thumbnail(track) or artwork, video_id)
    return 0


def main():
    if len(sys.argv) < 3:
        _line("ERROR", "Usage: ytmusic_bridge.py search-tracks <limit> <query> | search-albums <query> | album-tracks <browse-id>")
        return 2
    if sys.argv[1] == "search-tracks":
        if len(sys.argv) < 4:
            _line("ERROR", "search-tracks requires a limit and query")
            return 2
        return search_tracks(" ".join(sys.argv[3:]).strip(), sys.argv[2])
    if sys.argv[1] == "search-albums":
        return search_albums(" ".join(sys.argv[2:]).strip())
    if sys.argv[1] == "album-tracks":
        return album_tracks(sys.argv[2])
    _line("ERROR", "Unknown command")
    return 2


if __name__ == "__main__":
    sys.exit(main())
