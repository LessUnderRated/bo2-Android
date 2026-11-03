// ocr.swift <image> — lit le texte d'une photo du jeu (Vision, OCR local) : une ligne par texte,
// « x y texte », x y = centre normalisé 0..1, origine en haut à gauche (le repère de scripts/sendclick).
// Construire : swiftc -O tools/ocr.swift -o tools/ocr
import Foundation
import Vision

guard CommandLine.arguments.count == 2,
      let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: CommandLine.arguments[1]) as CFURL, nil),
      let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
    FileHandle.standardError.write("usage: ocr <image>\n".data(using: .utf8)!); exit(2)
}
let req = VNRecognizeTextRequest()
req.recognitionLevel = .fast
req.revision = VNRecognizeTextRequestRevision1   // ancien modèle : le neuf (e5rt) échoue ici
req.usesLanguageCorrection = false
req.usesCPUOnly = true   // le moteur neuronal (e5rt) échoue ici, erreur 13
try VNImageRequestHandler(cgImage: img).perform([req])
for o in req.results ?? [] {
    guard let t = o.topCandidates(1).first?.string else { continue }
    let b = o.boundingBox
    print(String(format: "%.3f %.3f %@", b.midX, 1 - b.midY, t))
}
