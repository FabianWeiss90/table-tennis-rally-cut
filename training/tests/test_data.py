# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

import numpy as np
import pytest

from tests.synthetic import FRAMES_PER_ROW, write_video
from ttrally_training.data import DataError, Rally, load_groups, row_labels, select_videos


def test_rows_are_labelled_by_their_frame():
    frames = np.array([0, 6, 12, 18, 24])
    labels = row_labels(frames, [Rally(6, 13), Rally(24, 30)])
    assert labels.tolist() == [0, 1, 1, 0, 1]


def test_only_completely_reviewed_videos_with_features_are_used(tmp_path):
    rallies = write_video(tmp_path, "done", 1500, seed=1)
    write_video(tmp_path, "half", 1500, seed=2, review="incomplete")
    write_video(tmp_path, "new", 1500, seed=3, review="missing")
    (tmp_path / "annotations" / "no_features.csv").write_text(
        (tmp_path / "annotations" / "done.csv").read_text())
    (tmp_path / "annotations" / "no_features.review.csv").write_text(
        (tmp_path / "annotations" / "done.review.csv").read_text())

    selection = select_videos(tmp_path / "features", tmp_path / "annotations")

    assert [v.video_id for v in selection.videos] == ["done"]
    assert "1 of 2 items open" in selection.skipped["half"]
    assert selection.skipped["new"] == "review not started"
    assert "no features" in selection.skipped["no_features"]
    video = selection.videos[0]
    # every rally counts, including the ones flagged let and aborted_toss
    assert len(video.rallies) == len(rallies)
    expected_rows = sum((end - start + 1) // FRAMES_PER_ROW for start, end in rallies)
    assert video.labels.sum() == expected_rows


def test_features_of_different_model_variants_are_not_mixed(tmp_path):
    write_video(tmp_path, "a", 1000, seed=1)
    write_video(tmp_path, "b", 1000, seed=2, model="fake-backbone")
    with pytest.raises(DataError, match="differ in model"):
        select_videos(tmp_path / "features", tmp_path / "annotations")


def test_groups_default_to_one_per_video(tmp_path):
    write_video(tmp_path, "a", 1000, seed=1)
    write_video(tmp_path, "b", 1000, seed=2)
    videos = select_videos(tmp_path / "features", tmp_path / "annotations").videos
    groups_file = tmp_path / "groups.csv"
    groups_file.write_text("video_id,group\na,channel\n")
    assert load_groups(None, videos) == {"a": "a", "b": "b"}
    assert load_groups(groups_file, videos) == {"a": "channel", "b": "b"}
