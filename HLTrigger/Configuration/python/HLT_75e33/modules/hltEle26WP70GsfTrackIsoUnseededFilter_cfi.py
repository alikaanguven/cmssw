import FWCore.ParameterSet.Config as cms

hltEle26WP70GsfTrackIsoUnseededFilter = cms.EDFilter("HLTEgammaGenericQuadraticEtaFilter",
    absEtaLowEdges = cms.vdouble(0.0, 1.0, 1.479, 2.1),
    candTag = cms.InputTag("hltEle26WP70GsfTrackIsoFromL1TracksUnseededFilter"),
    doRhoCorrection = cms.bool(False),
    effectiveAreas = cms.vdouble(0.029, 0.111, 0.114, 0.032),
    energyLowEdges = cms.vdouble(0.0),
    etaBoundaryEB12 = cms.double(1.0),
    etaBoundaryEE12 = cms.double(2.1),
    l1EGCand = cms.InputTag("hltEgammaCandidatesUnseeded"),
    lessThan = cms.bool(True),
    ncandcut = cms.int32(1),
    rhoMax = cms.double(99999999.0),
    rhoScale = cms.double(1.0),
    rhoTag = cms.InputTag("hltFixedGridRhoFastjetAllCaloForEGamma"),
    saveTags = cms.bool(True),
    thrOverE2EB1 = cms.vdouble(0.0),
    thrOverE2EB2 = cms.vdouble(0.0),
    thrOverE2EE1 = cms.vdouble(0.0),
    thrOverE2EE2 = cms.vdouble(0.0),
    thrOverEEB1 = cms.vdouble(0.0),
    thrOverEEB2 = cms.vdouble(0.0),
    thrOverEEE1 = cms.vdouble(0.0),
    thrOverEEE2 = cms.vdouble(0.0),
    thrRegularEB1 = cms.vdouble(1.76),
    thrRegularEB2 = cms.vdouble(1.76),
    thrRegularEE1 = cms.vdouble(1.76),
    thrRegularEE2 = cms.vdouble(1.76),
    useEt = cms.bool(True),
    varTag = cms.InputTag("hltEgammaEleGsfTrackIsoUnseeded")
)
