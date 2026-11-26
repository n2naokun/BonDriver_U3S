# BonDriver_U3S

SK-MTVU3S向けのWindows x64用BonDriverです。地デジ・BS・110度CSの受信と、複数台での同時受信に対応します。

## 導入

[GitHub Releases](https://github.com/n2naokun/BonDriver_U3S/releases) から `BonDriver_U3S-x64.zip` をダウンロードして展開します。

SK-MTVU3Sには事前にWinUSBドライバーの割り当てが必要です。未設定の場合は、[Zadig](https://zadig.akeo.ie/) などでSK-MTVU3S（USB ID `3275:9015`）を選び、WinUSBを割り当てます。

1. `U3SHost.exe`、`U3SHost.ini`、登録・解除用のCMDを同じフォルダーに置きます。TVTestを使うユーザーの通常権限で `InstallU3SHost.cmd` を実行すると、Hostが起動し、そのユーザーの次回ログオンから自動起動します。今回だけ起動する場合は `U3SHost.exe` を直接実行します。
2. TVTest x64のBonDriver検索フォルダーに、`BonDriver_U3S.dll` と `BonDriver_U3S.ini` を置きます。DLLとINIは同じフォルダー・ベース名にします。
3. WinUSBを割り当てたSK-MTVU3Sを接続し、TVTestで `BonDriver_U3S.dll` を選択します。
4. チャンネルスキャンで受信したい空間を選びます。同梱INIにはBS・110度CSの選局表が入っています。地デジは設定不要で、物理13〜52chから受信できる局をTVTestが登録します。以前の地デジスキャン結果がある場合は再スキャンしてください。

選局表の編集方法はINIのコメントに記載しています。編集後はTVTestでBonDriverを開き直し、変更した空間を再スキャンします。このDLLの出力には放送側のMULTI2スクランブルが残るため、スクランブル付きの放送を視聴するには、TVTestやProxyExD側に対応する復号環境が必要です。

複数台を使う場合は、同時受信する台数分のDLLとINIを `BonDriver_U3S_2.dll` と `BonDriver_U3S_2.ini` のように別名で用意します。Hostが受信枠の空いている個体を割り当てます。旧INIの `InstanceId` は無視します。各個体が同じアンテナ構成で受信できるように接続してください。

DLLの読み込みに失敗する場合は、TVTestがx64版か、Microsoft Visual C++再頒布可能パッケージ（x64）が入っているかを確認してください。INIや選局のエラーはWindowsのデバッグ出力に記録します。

自動起動は `UninstallU3SHost.cmd` で解除できます。登録を解除しても、稼働中のHostはログオフまたはプロセスを終了するまで利用できます。

### ProxyExDサービスから接続する

Hostを普段使うユーザーで起動します。LocalSystemのProxyExDは、既定でコンソールにログオン中のユーザーのHostに接続します。接続先を固定する場合は、管理者が64ビットの `HKLM\SOFTWARE\U3SHost` に文字列値 `HostSid` として対象ユーザーのSIDを設定します。TVTestとProxyExDが同時に直接受信する場合は、受信要求ごとに別の個体が割り当てられます。

- **固定指定が不要**なのは、LocalSystemのProxyExDと組み合わせても、物理コンソールにログオン中のユーザーがHostを起動している場合です。TVTestがBonDriverを直接開く場合は、TVTestと同じユーザーでHostを起動すれば利用できます。
- **固定指定が必要**なのは、LocalSystemのProxyExDから、物理コンソールのログオンユーザーとは別のユーザーで起動済みのHostに接続する場合や、コンソールユーザーが切り替わっても同じHostを選びたい場合です。ProxyExDをLocalServiceまたはNetworkServiceで動かす場合も、接続先の `HostSid` を指定します。

Hostを起動するユーザーで `Create-ProxyExD-HostSid.cmd` を実行すると、同じフォルダーに登録用と解除用のREGが生成されます。管理者権限で `Set-ProxyExD-HostSid.reg` を取り込むと固定指定が有効になります。`Remove-ProxyExD-HostSid.reg` を取り込むと `HostSid` の値だけが削除され、LocalSystemでは次回の接続から自動検出に戻ります。固定指定はHostの起動や自動起動の登録とは別です。配布時は生成用CMDを同梱し、登録用REGは利用者のPCで生成します。

## 内蔵カードリーダーを使う場合

配布ZIPの `card-reader/winscard.dll` は、U3SHost経由で内蔵カードリーダーを使うx64用DLLです。HostがチューナーとカードへのUSBアクセスを管理するため、同じ個体で受信とカード通信を併用できます。

対応する復号器で、このDLLのパスを指定して読み込みます。WindowsのシステムDLLは上書きしないでください。外部カードリーダーを利用する構成では、このDLLの導入は不要です。

カードの共有方式と接続上限は `U3SHost.ini` の `[Card]` で設定します。各項目の説明と設定例はINI内のコメントを参照してください。

## ソースからビルド

Windows上でCMake 3.21以降と、MSVCのC++デスクトップ開発環境を使用します。Visual Studioのジェネレーターでx64を指定します。

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

BonDriverとHostは `build/Release/`、内蔵カードリーダー用DLLは `build/card-reader/Release/` に出力されます。配布ZIPを使う場合はビルド不要です。

## ライセンス

独自部分は [MITライセンス](LICENSE) です。[recfriio](https://github.com/waya0125/recfriio) から移植した部分の著作権表示と条件は [LICENSE.recfriio](LICENSE.recfriio) に記載しています。バイナリを配布する際は両方のライセンスを同梱してください。
