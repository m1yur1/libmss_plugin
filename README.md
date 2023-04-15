# libmss_plugin
[VLC media player](https://www.videolan.org/vlc/) 3.0.x 向けの、[Microsoft Spatial Sound](https://learn.microsoft.com/en-us/windows/win32/coreaudio/spatial-sound) を利用するオーディオ出力プラグイン。  

## 動作確認環境  
VLC media player Version 3.0.18 Windows 64bit  
Microsoft Windows [Version 10.0.22621.1555]  
Windows Sonic for Headphones  

## ビルド環境  
Visual Studio 2022 v17.5.4  
VLC media player SDK Version 3.0.18  
Windows Implementation Libraries v1.0.230411.1  

### ビルド時の注意点
```
MODULE_STRING="libmss_plugin"
__PLUGIN__
``` 
を定義すること。  

## 使用方法
### 立体音響方式の選択  
設定 で、システム > サウンド と進み、下にある『サウンドの詳細設定』をクリックする。  
ウィンドウタイトルが サウンド のウィンドウが出るので、『再生』タブの中から設定したいデバイスをダブルクリックする。  
『立体音響』タブ内で適用する立体音響方式を選び、右下の『適用』ボタンを押す。  

### VLC media player での設定
libmss_plugin.dll を plugins\audio_output ディレクトリにコピーする。  
VLC media player を起動し、メニューから『ツール (S)』、『設定 (P)』を選択し、『シンプルな設定』ウィンドウを出す。  
左下の『設定の表示』グループの『すべて』を選択し、『詳細設定』ウィンドウに変更する。  
左の『オーディオ』→『出力モジュール』を選択し、右の『オーディオ出力モジュール』を Microsoft Spatial Sound audio output にする。  
左の『オーディオ』→『出力モジュール』→『MSS』を選択し、右の『出力デバイス』を適切に設定する。  
また、その下の各初期値は、オーディオ 音量:=0.5、Audio mute:=uncheck、Wait Timeout:=10、Flush Wait:=0、Stop Wait:=10 である。  
右下の『保存 (S)』ボタンを押す。 
VLC media player を終了し、再度起動する。  
