# Windows test qualification

The Wave 4 candidate uses upstream 1e19e095 with two test-only corrections.
No launcher implementation changes are included.

The PSX binding fixture now reads the text INI in text mode. Its exact adjacent
Circle=None and Cross=S expectation remains. Windows writes CRLF separators.
The wizard fixture compares the stored path with its canonical spelling.
It still requires the chosen BIOS, replacement selection and original revert target.
Path canonicalization itself is outside this staging test.

The unchanged upstream fixtures failed these three assertions on Windows.
The full CTest suite must pass after these portability corrections.
