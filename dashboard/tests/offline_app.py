"""加载生产函数与路由；排除启动表达式、dotenv和磁盘数据库。"""
import ast
import os
from pathlib import Path
import sqlite3
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / "server.py"


def create_app():
    tree = ast.parse(SOURCE.read_text(encoding="utf-8"))
    nodes = [n for n in tree.body if isinstance(n, (ast.Import, ast.ImportFrom, ast.Assign, ast.FunctionDef, ast.ClassDef))
             and not (isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "_conn" for t in n.targets))]
    ns = {"__file__": str(SOURCE), "__name__": "dashboard_offline"}
    with patch.dict(os.environ, {}, clear=True):
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), "exec"), ns)
    db = sqlite3.connect(":memory:", check_same_thread=False)
    db.row_factory = sqlite3.Row
    ns["_conn"] = db
    ns["init_db"](db)
    app = ns["app"]
    app.template_folder = str(SOURCE.parent / "templates")
    app.static_folder = str(SOURCE.parent / "static")
    app.config["TESTING"] = True
    return app, ns, db
