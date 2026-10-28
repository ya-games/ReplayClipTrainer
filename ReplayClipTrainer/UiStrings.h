#pragma once
// Strings of the settings window and the on-screen overlays, English + Japanese (UTF-8).
// RCT_UI_STRINGS is an X-macro so the constants and ui::kAllJa (used to build the Japanese font's glyph ranges) come from one
// list: add a string here and its glyphs are loaded automatically. (Fonts are not rebuilt on a plugin hot-reload - restart
// the game after adding Japanese characters.)
// Strings that contain %d / %s are printf formats, filled in by the caller.
#define RCT_UI_STRINGS(X) \
	X(tabConvert,   "CONVERT",    u8"変換") \
	X(tabClips,     "CLIPS",      u8"クリップ") \
	X(tabSettings,  "SETTINGS",   u8"設定") \
	X(caption,      "REPLAY",     u8"REPLAY") \
	X(title,        "CLIP TRAINER", u8"CLIP TRAINER") \
	X(stIdle,       "IDLE",       u8"待機中") \
	X(stReplay,     "REPLAY",     u8"リプレイ中") \
	X(stConverting, "CONVERTING", u8"変換中") \
	X(stPreparing,  "PREPARING",  u8"準備中") \
	X(stTraining,   "TRAINING",   u8"練習中") \
	X(howTo,        "While watching a replay, scrub to the moment you want, then press Train Now to train it right away, or Save for Later to keep it.", u8"リプレイを再生し、練習したい瞬間までスクラブしてから、「今すぐ練習」でそのまま練習、「あとで練習(保存)」で保存します。") \
	X(warnChange,   "Do not change teams, restart the match or go to Spectate while converting.", u8"変換中は、チーム変更・試合の再開・観戦への切り替えをしないでください。") \
	X(warnConverting, "A clip is being converted - please wait (or press Cancel on the progress bar).", u8"クリップを変換中です。完了までお待ちください(進行バーのキャンセルも可)。") \
	X(notInReplay,  "Open a replay to convert a clip from it.", u8"リプレイを開くと、ここからクリップを変換できます。") \
	X(lblPosition,  "REPLAY POSITION", u8"リプレイの位置") \
	X(lblRange,     "CLIP RANGE", u8"切り出し範囲") \
	X(lblBefore,    "Seconds before the scrub position", u8"スクラブ位置の何秒前から") \
	X(lblAfter,     "Seconds after the scrub position", u8"スクラブ位置の何秒後まで") \
	X(lblRes,       "Conversion resolution", u8"変換の細かさ(解像度)") \
	X(resHint,      "Lower = smoother motion, but the conversion takes longer.", u8"小さいほど動きが滑らかになりますが、変換に時間がかかります。") \
	X(btnTrainNow,  "Train Now", u8"今すぐ練習") \
	X(btnTrainNowD, "Cut a short clip around the current scrub position and start a private match right away.", u8"現在のスクラブ位置の前後を切り出し、すぐにプライベートマッチで練習を始めます。") \
	X(btnSave,      "Save for Later", u8"あとで練習(保存)") \
	X(btnSaveD,     "Save the clip to a file without opening a match - store several scenes from one replay and pick one to train later.", u8"マッチを開かずにクリップをファイルへ保存します。1本のリプレイから複数の場面を保存し、あとで選んで練習できます。") \
	X(phName,       "Clip name (optional - auto-named if blank)", u8"クリップ名(空欄なら自動で命名)") \
	X(lblSave,      "SAVE CLIP", u8"クリップを保存") \
	X(lblQuick,     "TRAIN WITHOUT SAVING", u8"保存せずにすぐ練習") \
	X(stAuto,       "AUTO-PLAY", u8"自動再生中") \
	X(stDriving,    "YOU'RE DRIVING", u8"操作中") \
	X(hudPress,     "Press: \"%s\"", u8"押すと「%s」") \
	X(ctlJumpIn,    "Take over", u8"自分で操作") \
	X(ctlJumpOut,   "Resume playback", u8"自動再生に戻す") \
	X(tipGoal,      "Goal: %s  (%d:%02d)", u8"ゴール: %s  (%d:%02d)") \
	X(tipHighlight, "Highlight  (%d:%02d)", u8"ハイライト  (%d:%02d)") \
	X(btnNewFolder, "New folder", u8"新しいフォルダ") \
	X(phFolderName, "Folder name", u8"フォルダ名") \
	X(phClipName,   "Clip name", u8"クリップ名") \
	X(btnCreate,    "Create", u8"作成") \
	X(btnRename,    "Rename", u8"名前を変更") \
	X(dlgNewFolder, "NEW FOLDER", u8"新しいフォルダ") \
	X(dlgClip,      "CLIP OPTIONS", u8"クリップの操作") \
	X(dlgFolder,    "FOLDER OPTIONS", u8"フォルダの操作") \
	X(lblMoveTo,    "MOVE TO", u8"移動先") \
	X(txtRemain,    "%d:%02d left", u8"残り %d:%02d") \
	X(lblSaveTo,   "Save to folder", u8"保存先フォルダ") \
	X(txtCurrent,   "(current)", u8"(現在地)") \
	X(btnDeleteFolder, "Delete folder", u8"フォルダを削除") \
	X(btnConfirmFolder, "Confirm delete folder", u8"削除する") \
	X(errExists,    "A file or folder with that name already exists.", u8"同じ名前のファイルまたはフォルダが既にあります。") \
	X(errInvalid,   "That name can't be used.", u8"その名前は使えません。") \
	X(errNotEmpty,  "The folder is not empty - move or delete its clips first.", u8"フォルダが空ではありません。中のクリップを移動または削除してください。") \
	X(errFailed,    "The operation failed.", u8"操作に失敗しました。") \
	X(guideText,    "Left / Right : back 5 s / forward 5 s\nUp / Down : switch player\nB : pause / resume\nV : take over / back to auto-play\nPad : D-pad / L3 / Back work the same way", u8"← / → : 5秒戻る / 5秒進む\n↑ / ↓ : 選手の切り替え\nB : 一時停止 / 再開\nV : 自分で操作 / 自動再生に戻す\nパッド : 十字キー / L3 / Back で同様に操作") \
	X(tgKeyGuide,   "Show the key guide on screen", u8"画面にキー操作の説明を表示") \
	X(btnReopen,    "Reopen the last converted replay", u8"直前に変換したリプレイを開く") \
	X(reopenHint,   "Opens the replay you last converted from, a few seconds before the last clip, so you can cut another scene.", u8"最後に変換したリプレイを、直前のクリップの少し前から開き直します。別の場面を切り出せます。") \
	X(noSourceReplay, "No converted replay yet - nothing to reopen.", u8"変換したリプレイがまだないため、開けません。") \
	X(tgAnon,       "Anonymize player names when saving", u8"保存時に選手名を匿名化") \
	X(anonD,        "Names become Player1, 2, 3...: the player you were spectating is Player1, then their teammates, then the opponents (1v1 to 3v3).", u8"選手名を Player1, 2, 3... に置き換えます。観戦していた選手が Player1、次に味方、最後に相手の順です(1v1〜3v3対応)。") \
	X(errShort,     "Clip is too short - Before + After must add up to at least %d seconds.", u8"クリップが短すぎます。「何秒前」と「何秒後」の合計を %d 秒以上にしてください。") \
	X(savedOk,      "Saved clip: %s", u8"保存しました: %s") \
	X(savedFail,    "Failed to save clip to %s", u8"保存に失敗しました: %s") \
	X(lblClips,     "SAVED CLIPS", u8"保存したクリップ") \
	X(btnRefresh,   "Refresh", u8"更新") \
	X(btnOpenFolder, "Open folder", u8"フォルダを開く") \
	X(btnRetry,     "Retry last clip", u8"直前のクリップをやり直す") \
	X(noRetry,      "No clip converted or loaded yet - nothing to retry.", u8"変換・読み込み済みのクリップがないため、やり直せません。") \
	X(tgRecent,     "Recent clips only", u8"最近のクリップのみ") \
	X(txtHours,     "h",          u8"時間") \
	X(noClips,      "No saved clips yet.", u8"保存したクリップはまだありません。") \
	X(noRecent,     "No clips saved within the selected time.", u8"選択した期間内に保存したクリップはありません。") \
	X(btnLoad,      "Load",       u8"読み込み") \
	X(btnDelete,    "Delete",     u8"削除") \
	X(btnConfirm,   "Confirm delete", u8"削除する") \
	X(btnCancel,    "Cancel",     u8"キャンセル") \
	X(btnBack,      "Back",       u8"戻る") \
	X(txtIncompat,  "incompatible", u8"非対応") \
	X(txtRoot,      "(root)",     u8"(ルート)") \
	X(loadFail,     "Failed to load clip", u8"クリップの読み込みに失敗しました") \
	X(lblTraining,  "TRAINING",   u8"練習中の設定") \
	X(tgLimitedBoost, "Limited boost in replay", u8"ブーストを制限する") \
	X(tgInputUnpause, "Unpause on controller input when jumping in", u8"JumpIn時、操作入力で一時停止を解除") \
	X(tgShowHud,    "Show HUD",   u8"HUDを表示") \
	X(tgDisableGoal, "Disable goal replay / explosion", u8"ゴール演出(リプレイ・爆発)を無効化") \
	X(tgBallFree,   "Let the ball go free on resume (experimental)", u8"再開時にボールを実物理へ解放(実験的)") \
	X(ballFreeD,    "While on, resuming from pause hands the ball to real physics instead of the recorded path - useful for touches later than in the original.", u8"オンにすると、一時停止から再開した瞬間にボールが記録の軌道を離れ、実際の物理で動きます。記録より遅いタッチの練習に使えます。") \
	X(lblBindings,  "KEY BINDINGS", u8"キー割り当て") \
	X(btnApplyBind, "Apply standard bindings", u8"標準の割り当てを適用") \
	X(bindDesc,     "Right / DPad Right: skip 5 s    Left / DPad Left: back 5 s    Up / DPad Up: next player    Down / DPad Down: previous player    B / L3: pause    V / Back: JumpIn. Change keys in BakkesMod's Bindings tab.", u8"右 / 十字右: 5秒進む    左 / 十字左: 5秒戻る    上 / 十字上: 次の選手    下 / 十字下: 前の選手    B / L3: 一時停止    V / Back: JumpIn。キーの変更はBakkesModのBindingsタブで行います。") \
	X(lblLang,      "LANGUAGE",   u8"言語") \
	X(fontReady,    "Japanese font ready", u8"日本語フォント: 読み込み済み") \
	X(fontLoading,  "Japanese font not loaded (restart the game if this stays)", u8"日本語フォント: 未読み込み(このままならゲームを再起動)") \
	X(lblWindow,    "WINDOW",     u8"ウィンドウ") \
	X(lblSize,      "Size",       u8"大きさ") \
	X(lblOpacity,   "Opacity",    u8"不透明度") \
	X(lblAbout,     "ABOUT",      u8"情報") \
	X(aboutBy,      "ReplayClipTrainer by ya-games (github.com/ya-games)", u8"ReplayClipTrainer by ya-games (github.com/ya-games)") \
	X(aboutBase,    "Based on JumpInReplay by Atomus48 (github.com/Atomus48/JumpInReplay)", u8"Based on JumpInReplay by Atomus48 (github.com/Atomus48/JumpInReplay)") \
	X(txtVersion,   "Version",    u8"バージョン") \
	X(hintDrag,     "Drag an empty area to move the window", u8"空いている場所をドラッグして移動") \
	X(ovPreparing,  "Preparing training session", u8"練習セッションを準備しています") \
	X(ovPaused,     "The replay is paused - unpause it in the game to continue the conversion.", u8"リプレイが一時停止中です。ゲーム側で一時停止を解除すると変換が進みます。") \
	X(ovConverting, "Converting replay...", u8"リプレイを変換中...") \
	X(cmpAsk,       "The plugin %s is incompatible with ReplayClipTrainer. Disable it?", u8"プラグイン %s は ReplayClipTrainer と互換性がありません。無効化しますか?") \
	X(cmpDisable,   "Disable it", u8"無効化する") \
	X(cmpKeep,      "Keep it",    u8"無効化しない") \
	X(cmpNoAsk,     "Do not ask again", u8"今後は確認しない")

namespace ui {
struct S { const char* en; const char* ja; };

#define X(id, en, ja) inline constexpr S id{ en, ja };
RCT_UI_STRINGS(X)
#undef X

inline constexpr const char* kAllJa[] = {
#define X(id, en, ja) ja,
RCT_UI_STRINGS(X)
#undef X
};
} // namespace ui
